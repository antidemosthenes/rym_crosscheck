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
                                     std::size_t workers) {
    workers = std::min(workers, max_web_workers);
    std::vector<std::size_t> jobs;
    for (const auto &row : missing)
        if (row.song.album.empty())
            jobs.push_back(row.song.rank - 1);
    std::vector<std::string> failures(jobs.size());
    std::atomic_size_t next{};
    std::atomic_bool blocked{};
    std::vector<std::jthread> threads;
    for (std::size_t i = 0; i < std::min(workers, jobs.size()); ++i)
        threads.emplace_back([&] {
            try {
                auto transport = factory();
                while (!blocked.load()) {
                    auto task = next.fetch_add(1);
                    if (task >= jobs.size())
                        break;
                    check_cancelled();
                    auto &song = snapshot.songs[jobs[task]];
                    for (unsigned attempt = 1; attempt <= 3 && !blocked.load(); ++attempt) {
                        check_cancelled();
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
                            failures[task] = error.what();
                            blocked.store(true);
                            break;
                        } catch (const SignInRequired &error) {
                            failures[task] = error.what();
                            blocked.store(true);
                            break;
                        } catch (const std::exception &error) {
                            check_cancelled();
                            failures[task] = error.what();
                        }
                        if (attempt < 3 && !blocked.load())
                            log("Retrying album lookup (" + std::to_string(attempt + 1) +
                                "/3): " + song.title);
                    }
                }
            } catch (const std::exception &error) {
                log("Album lookup warning: " + std::string(error.what()));
                blocked.store(true);
            }
        });
    threads.clear();
    check_cancelled();
    std::vector<std::string> warnings;
    for (std::size_t task = 0; task < jobs.size(); ++task) {
        const auto &song = snapshot.songs[jobs[task]];
        if (!song.album.empty())
            continue;
        auto reason = failures[task].empty() ? "Lookup stopped before album metadata was available."
                                              : failures[task];
        auto warning = "Album unavailable for #" + std::to_string(song.rank) + " " + song.title +
                       ": " + reason;
        log(warning);
        warnings.push_back(std::move(warning));
    }
    return warnings;
}
}
