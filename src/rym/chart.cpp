#include "rym_crosscheck/rym/chart.hpp"
#include "rym_crosscheck/rym/parser.hpp"
#include <algorithm>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

namespace rym {
Snapshot fetch_chart(Transport &transport, std::size_t top) {
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
void enrich_albums(const std::vector<Missing> &missing, Snapshot &snapshot,
                   const std::function<std::unique_ptr<Transport>()> &factory,
                   std::size_t workers) {
    workers = std::min(workers, max_web_workers);
    std::vector<std::size_t> jobs;
    for (const auto &row : missing)
        if (row.song.album.empty())
            jobs.push_back(row.song.rank - 1);
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
                    try {
                        song.album = parse_album(transport->fetch(song.url));
                        if (song.album.empty())
                            log("Album unavailable or ambiguous: " + song.title);
                    } catch (const std::exception &error) {
                        log("Album lookup warning: " + std::string(error.what()));
                        blocked.store(true);
                    }
                }
            } catch (const std::exception &error) {
                log("Album lookup warning: " + std::string(error.what()));
                blocked.store(true);
            }
        });
    threads.clear();
    check_cancelled();
}
}
