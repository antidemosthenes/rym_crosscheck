#include "rym_crosscheck/library/scanner.hpp"
#include "rym_crosscheck/matching/normalize.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <utility>

namespace rym {
Track read_track(const std::filesystem::path &path) {
    check_cancelled();
    Track track;
    track.path = std::filesystem::absolute(path);
    track.size = std::filesystem::file_size(path);
    track.modified = modified_time(path);
    TagLib::FileRef file(path.c_str(), false);
    if (!file.isNull() && file.tag()) {
        track.title = file.tag()->title().to8Bit(true);
        track.album = file.tag()->album().to8Bit(true);
        auto properties = file.file()->properties();
        if (properties.contains("ARTIST"))
            for (const auto &artist : properties["ARTIST"])
                if (!artist.isEmpty())
                    track.artists.push_back(artist.to8Bit(true));
        if (track.artists.empty() && !file.tag()->artist().isEmpty())
            track.artists.push_back(file.tag()->artist().to8Bit(true));
    }
    if (track.title.empty() || track.artists.empty()) {
        auto filename = path_text(path.stem());
        auto separator = filename.find(" - ");
        if (separator != filename.npos) {
            if (track.artists.empty())
                track.artists.push_back(filename.substr(0, separator));
            if (track.title.empty())
                track.title = filename.substr(separator + 3);
        }
    }
    identity(track.title, track.artists);
    if (track.size != std::filesystem::file_size(path) || track.modified != modified_time(path))
        throw Error("File changed while reading tags.");
    return track;
}
Scan scan_library(const std::filesystem::path &root, const Cache &cache, std::size_t worker_count) {
    if (!std::filesystem::is_directory(root))
        throw Error("Music directory is not readable.");
    auto reused = cache.load_tracks(root);
    struct State {
        std::mutex mutex;
        std::condition_variable ready, space;
        std::deque<std::filesystem::path> queue;
        bool done{};
        bool stop{};
        Scan scan;
    } state;
    auto worker = [&] {
        for (;;) {
            std::filesystem::path path;
            {
                std::unique_lock lock(state.mutex);
                state.ready.wait_for(lock, std::chrono::milliseconds(100), [&] {
                    return !state.queue.empty() || state.done || state.stop || cancelled.load();
                });
                if (state.stop || cancelled.load())
                    return;
                if (state.queue.empty()) {
                    if (state.done)
                        return;
                    continue;
                }
                path = std::move(state.queue.front());
                state.queue.pop_front();
                state.space.notify_one();
            }
            try {
                Track track;
                bool hit{};
                const auto found = std::as_const(reused).find(path_text(path));
                if (found != reused.end() &&
                    found->second.size == std::filesystem::file_size(path) &&
                    found->second.modified == modified_time(path)) {
                    track = found->second;
                    identity(track.title, track.artists);
                    hit = true;
                } else
                    track = read_track(path);
                std::lock_guard lock(state.mutex);
                state.scan.tracks.push_back(std::move(track));
                if (hit)
                    ++state.scan.reused;
            } catch (const std::exception &error) {
                std::lock_guard lock(state.mutex);
                state.scan.warnings.push_back(path_text(path) + ": " + error.what());
            }
        }
    };
    std::vector<std::jthread> workers;
    for (std::size_t i = 0; i < worker_count; ++i)
        workers.emplace_back(worker);
    try {
        std::error_code error;
        auto iterator = std::filesystem::recursive_directory_iterator(
            root, std::filesystem::directory_options::none, error);
        if (error)
            throw Error("Cannot enumerate music directory: " + error.message());
        while (iterator != std::filesystem::recursive_directory_iterator{}) {
            check_cancelled();
            const auto entry = *iterator;
            std::error_code status_error;
            const auto status = entry.symlink_status(status_error);
            if (status_error) {
                std::lock_guard lock(state.mutex);
                state.scan.warnings.push_back(path_text(entry.path()) + ": " +
                                              status_error.message());
            } else if (std::filesystem::is_symlink(status) ||
                       (GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))
                iterator.disable_recursion_pending();
            else if (std::filesystem::is_regular_file(status)) {
                auto extension = normalize(path_text(entry.path().extension())).text;
                if (extension == ".mp3" || extension == ".flac" || extension == ".m4a") {
                    std::unique_lock lock(state.mutex);
                    while (state.queue.size() >= worker_count * 4) {
                        state.space.wait_for(lock, std::chrono::milliseconds(100));
                        check_cancelled();
                    }
                    state.queue.push_back(std::filesystem::absolute(entry.path()));
                    state.ready.notify_one();
                }
            }
            iterator.increment(error);
            if (error) {
                std::lock_guard lock(state.mutex);
                state.scan.warnings.push_back("Directory traversal incomplete: " + error.message());
                error.clear();
            }
        }
        {
            std::lock_guard lock(state.mutex);
            state.done = true;
        }
        state.ready.notify_all();
        workers.clear();
        check_cancelled();
    } catch (...) {
        {
            std::lock_guard lock(state.mutex);
            state.stop = true;
        }
        state.ready.notify_all();
        state.space.notify_all();
        workers.clear();
        throw;
    }
    std::ranges::sort(state.scan.tracks,
                      [](const Track &a, const Track &b) { return a.path < b.path; });
    std::ranges::sort(state.scan.warnings);
    return std::move(state.scan);
}
}
