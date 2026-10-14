#include "rym_crosscheck/storage/cache.hpp"
#include "rym_crosscheck/rym/parser.hpp"
#include <algorithm>

namespace rym {
namespace {
constexpr std::int64_t schema = 1;
std::filesystem::path prepare(std::filesystem::path path) {
    path = std::filesystem::absolute(path).lexically_normal();
    std::filesystem::create_directories(path);
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
        throw Error("Cache directory cannot be a symbolic link.");
    return path;
}
boost::json::array strings(const std::vector<std::string> &values) {
    boost::json::array result;
    for (const auto &v : values)
        result.emplace_back(boost::json::string_view(v.data(), v.size()));
    return result;
}
std::vector<std::string> strings(const boost::json::value &value) {
    std::vector<std::string> result;
    for (const auto &v : value.as_array())
        result.emplace_back(v.as_string().c_str());
    return result;
}
std::string root_key(const std::filesystem::path &root) {
    return path_text(std::filesystem::weakly_canonical(root));
}
}
Cache::Cache(std::filesystem::path root, bool clear)
    : root_(prepare(std::move(root))), lock_(root_ / "run.lock") {
    if (clear)
        for (const auto *name : {"tracks.json", "chart.json"}) {
            std::error_code error;
            std::filesystem::remove(root_ / name, error);
        }
    prune();
}
boost::unordered_flat_map<std::string, Track>
Cache::load_tracks(const std::filesystem::path &root) const {
    boost::unordered_flat_map<std::string, Track> result;
    auto file = root_ / "tracks.json";
    if (!std::filesystem::exists(file))
        return result;
    try {
        auto data = read_json(file).as_object();
        if (data.at("schema").as_int64() != schema || data.at("root").as_string() != root_key(root))
            return result;
        for (const auto &entry : data.at("tracks").as_array()) {
            const auto &v = entry.as_object();
            Track track;
            track.path = wide(v.at("path").as_string().c_str());
            track.title = v.at("title").as_string().c_str();
            track.artists = strings(v.at("artists"));
            track.album = v.at("album").as_string().c_str();
            track.size = v.at("size").to_number<std::uintmax_t>();
            track.modified = v.at("modified").as_int64();
            result.emplace(path_text(track.path), std::move(track));
        }
    } catch (const std::exception &error) {
        log("Ignoring unusable tag cache: " + std::string(error.what()));
        result.clear();
    }
    return result;
}
void Cache::save_tracks(const std::filesystem::path &root, const std::vector<Track> &tracks) const {
    boost::json::array entries;
    entries.reserve(tracks.size());
    for (const auto &track : tracks)
        entries.push_back(boost::json::object{{"path", path_text(track.path)},
                                              {"title", track.title},
                                              {"artists", strings(track.artists)},
                                              {"album", track.album},
                                              {"size", track.size},
                                              {"modified", track.modified}});
    write_json(root_ / "tracks.json", boost::json::object{{"schema", schema},
                                                          {"root", root_key(root)},
                                                          {"tracks", std::move(entries)}});
    prune();
}
void Cache::save_snapshot(const Snapshot &snapshot) const {
    boost::json::array entries;
    entries.reserve(snapshot.songs.size());
    for (const auto &song : snapshot.songs)
        entries.push_back(boost::json::object{{"rank", song.rank},
                                              {"title", song.title},
                                              {"artists", strings(song.artists)},
                                              {"album", song.album},
                                              {"url", song.url}});
    write_json(root_ / "chart.json", boost::json::object{{"schema", schema},
                                                         {"captured", snapshot.captured},
                                                         {"source", snapshot.source},
                                                         {"songs", std::move(entries)}});
    prune();
}
Snapshot Cache::load_snapshot(std::size_t top) const {
    auto file = root_ / "chart.json";
    if (!std::filesystem::exists(file))
        throw Error("No cached chart available; complete a live run first.");
    auto data = read_json(file).as_object();
    if (data.at("schema").as_int64() != schema)
        throw Error("Cached chart version is incompatible.");
    Snapshot snapshot;
    snapshot.captured = data.at("captured").as_int64();
    snapshot.source = data.at("source").as_string().c_str();
    for (const auto &entry : data.at("songs").as_array()) {
        const auto &v = entry.as_object();
        snapshot.songs.push_back({v.at("rank").to_number<std::size_t>(),
                                  v.at("title").as_string().c_str(), strings(v.at("artists")),
                                  v.at("album").as_string().c_str(),
                                  v.at("url").as_string().c_str()});
    }
    validate_chart(snapshot.songs, top);
    snapshot.songs.resize(top);
    return snapshot;
}
void Cache::prune() const {
    constexpr std::uintmax_t maximum = 128ULL * 1024 * 1024;
    std::vector<std::filesystem::directory_entry> files;
    std::uintmax_t total{};
    auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 30);
    for (const auto *name : {"tracks.json", "chart.json"}) {
        auto path = root_ / name;
        if (!std::filesystem::exists(path))
            continue;
        auto entry = std::filesystem::directory_entry(path);
        if (entry.is_symlink())
            throw Error("Cache files cannot be symbolic links.");
        if (entry.last_write_time() < cutoff) {
            std::filesystem::remove(path);
            continue;
        }
        total += entry.file_size();
        files.push_back(entry);
    }
    std::ranges::sort(files, [](const auto &a, const auto &b) {
        return a.last_write_time() < b.last_write_time();
    });
    for (const auto &file : files) {
        if (total <= maximum)
            break;
        auto size = file.file_size();
        std::filesystem::remove(file.path());
        total -= size;
    }
    for (const auto &entry : std::filesystem::directory_iterator(root_))
        if (entry.is_regular_file() && !entry.is_symlink() &&
            entry.path().filename().wstring().find(L".rym-") != std::wstring::npos &&
            entry.path().extension() == L".tmp") {
            std::error_code error;
            std::filesystem::remove(entry.path(), error);
        }
}
}
