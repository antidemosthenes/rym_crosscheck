#include "rym_crosscheck/rym/chart.hpp"
#include "rym_crosscheck/rym/parser.hpp"
#include <algorithm>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

namespace rym {
Snapshot fetch_chart(Transport &transport, std::size_t top,
                     const std::function<void(std::size_t)> &progress) {
    Snapshot snapshot;
    snapshot.captured = now_seconds();
    std::string current = snapshot.source;
    boost::unordered_flat_set<std::string> visited;
    boost::unordered_flat_map<std::size_t, Song> ranks;
    while (ranks.size() < top) {
        check_cancelled();
        if (!visited.insert(current).second)
            throw Error("Chart pagination loop detected.");
        log("Retrieving chart page " + current);
        auto page = parse_chart(transport.fetch(current), current);
        for (auto &song : page.songs)
            if (song.rank <= top) {
                auto found = ranks.find(song.rank);
                if (found != ranks.end()) {
                    if (found->second.url != song.url || found->second.title != song.title ||
                        found->second.artists != song.artists)
                        throw Error("Conflicting chart entries across pages.");
                } else
                    ranks.emplace(song.rank, std::move(song));
            }
        if (progress)
            progress(ranks.size());
        if (ranks.size() >= top)
            break;
        if (page.next.empty())
            throw Error("RYM pagination ended before the requested top N.");
        current = std::move(page.next);
    }
    snapshot.songs.reserve(ranks.size());
    for (auto &[rank, song] : ranks)
        snapshot.songs.push_back(std::move(song));
    validate_chart(snapshot.songs, top);
    return snapshot;
}
std::vector<std::string> enrich_albums(const std::vector<Missing> &missing, Snapshot &snapshot,
                                     const std::function<std::unique_ptr<Transport>()> &factory,
                                     std::size_t workers,
                                     const std::function<void(std::string_view, AccessReason)> &recover) {
    check_cancelled();
    if (!workers)
        throw Error("Album worker count must be positive.");
    workers = std::min(workers, max_web_workers);
    std::vector<std::size_t> jobs;
    jobs.reserve(missing.size());
    for (const auto &row : missing) {
        auto rank = row.song.rank;
        if (!rank || rank > snapshot.songs.size() || snapshot.songs[rank - 1].rank != rank ||
            snapshot.songs[rank - 1].url != row.song.url)
            throw Error("Album lookup row does not match the chart snapshot.");
        if (snapshot.songs[rank - 1].album.empty())
            jobs.push_back(rank - 1);
    }
    if (!std::ranges::is_sorted(jobs))
        std::ranges::sort(jobs);
    if (std::ranges::adjacent_find(jobs) != jobs.end())
        throw Error("Album lookup contains duplicate chart ranks.");
    if (jobs.empty())
        return {};
    if (!factory)
        throw Error("Album lookup requires a transport factory.");
    std::vector<std::string> failures(jobs.size());
    std::vector<unsigned> attempts(jobs.size());
    std::vector<std::size_t> pending;
    pending.reserve(jobs.size());
    bool verified{}, signed_in{};
    std::string stopped;
    for (;;) {
        pending.clear();
        for (std::size_t task = 0; task < jobs.size(); ++task)
            if (snapshot.songs[jobs[task]].album.empty() && attempts[task] < 3)
                pending.push_back(task);
        if (pending.empty())
            break;
        std::atomic_size_t next{};
        std::atomic_bool blocked{};
        std::mutex access_mutex;
        std::optional<std::pair<std::size_t, AccessReason>> access;
        std::string worker_failure;
        auto restrict = [&](std::size_t task, AccessReason reason, const std::exception &error) {
            failures[task] = error.what();
            {
                std::lock_guard lock(access_mutex);
                if (!access)
                    access.emplace(task, reason);
            }
            blocked.store(true);
        };
        std::vector<std::jthread> threads;
        threads.reserve(std::min(workers, pending.size()));
        for (std::size_t i = 0; i < std::min(workers, pending.size()); ++i)
            threads.emplace_back([&] {
                try {
                    if (blocked.load())
                        return;
                    check_cancelled();
                    auto transport = factory();
                    if (!transport)
                        throw Error("Album worker transport is unavailable.");
                    while (!blocked.load()) {
                        auto position = next.fetch_add(1);
                        if (position >= pending.size())
                            break;
                        auto task = pending[position];
                        auto &song = snapshot.songs[jobs[task]];
                        while (attempts[task] < 3 && !blocked.load()) {
                            check_cancelled();
                            ++attempts[task];
                            try {
                                auto html = transport->fetch(song.url);
                                if (challenged(html))
                                    throw VerificationRequired("RYM security check received during album lookup.");
                                auto album = parse_album(html);
                                if (!album.empty()) {
                                    song.album = std::move(album);
                                    break;
                                }
                                failures[task] = "No unique supported album metadata.";
                            } catch (const VerificationRequired &error) {
                                --attempts[task];
                                restrict(task, AccessReason::verification, error);
                                break;
                            } catch (const SignInRequired &error) {
                                --attempts[task];
                                restrict(task, AccessReason::sign_in, error);
                                break;
                            } catch (const RequestGroupStopped &) {
                                --attempts[task];
                                blocked.store(true);
                                break;
                            } catch (const std::exception &error) {
                                check_cancelled();
                                failures[task] = error.what();
                            }
                            if (attempts[task] < 3 && !blocked.load())
                                log("Retrying album lookup (" + std::to_string(attempts[task] + 1) +
                                    "/3): " + song.title);
                        }
                    }
                } catch (const std::exception &error) {
                    blocked.store(true);
                    {
                        std::lock_guard lock(access_mutex);
                        if (worker_failure.empty())
                            worker_failure = error.what();
                    }
                    log("Album lookup warning: " + std::string(error.what()));
                }
            });
        // Join every transport before access recovery resets the shared browser session.
        threads.clear();
        check_cancelled();
        if (!access) {
            if (blocked.load())
                stopped = worker_failure.empty()
                              ? "Album request group stopped without an available access recovery."
                              : "Album workers stopped: " + worker_failure;
            break;
        }
        const auto &[task, reason] = *access;
        const auto &song = snapshot.songs[jobs[task]];
        stopped = "RYM stopped the album request group at #" + std::to_string(song.rank) +
                  " " + song.title + " (" + song.url + ").";
        auto &recovered = reason == AccessReason::verification ? verified : signed_in;
        if (!recover || recovered) {
            if (recovered)
                stopped += " The album access recovery limit was reached.";
            break;
        }
        recovered = true;
        try {
            log(failures[task]);
            recover(song.url, reason);
            check_cancelled();
        } catch (const std::exception &error) {
            check_cancelled();
            stopped += " Access recovery failed: " + std::string(error.what());
            break;
        }
        workers = 1;
        stopped.clear();
    }
    std::vector<std::string> warnings;
    for (std::size_t task = 0; task < jobs.size(); ++task) {
        const auto &song = snapshot.songs[jobs[task]];
        if (!song.album.empty())
            continue;
        std::string_view reason = !stopped.empty() && attempts[task] < 3 ? stopped : failures[task];
        if (reason.empty())
            reason = "Lookup stopped before album metadata was available.";
        auto warning = "Album unavailable for #" + std::to_string(song.rank) + " " + song.title +
                       ": ";
        warning.append(reason);
        log(warning);
        warnings.push_back(std::move(warning));
    }
    return warnings;
}
}
