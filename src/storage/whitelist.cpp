#include "rym_crosscheck/storage/whitelist.hpp"
#include "rym_crosscheck/domain/runtime.hpp"
#include "rym_crosscheck/matching/normalize.hpp"
#include "rym_crosscheck/rym/parser.hpp"
#include <toml++/toml.hpp>

namespace rym {
Whitelist::Whitelist(const std::filesystem::path &path, bool required) {
    if (!std::filesystem::exists(path)) {
        if (required)
            throw Error("Whitelist does not exist: " + path_text(path));
        return;
    }
    auto table = toml::parse_file(path_text(path));
    for (const auto &[key, value] : table)
        if (key != "songs")
            throw Error("Unknown whitelist key: " + std::string(key.str()));
    auto *songs = table["songs"].as_array();
    if (!songs) {
        if (table.contains("songs"))
            throw Error("Whitelist songs must be an array of tables.");
        return;
    }
    for (const auto &entry : *songs) {
        const auto *row = entry.as_table();
        if (!row)
            throw Error("Each whitelist entry must be a table.");
        for (const auto &[key, value] : *row)
            if (key != "url" && key != "artist" && key != "artists" && key != "title")
                throw Error("Unknown whitelist entry key.");
        if (row->contains("url") && !(*row)["url"].is_string())
            throw Error("Whitelist URL must be a string.");
        if (row->contains("artist") && !(*row)["artist"].is_string())
            throw Error("Whitelist artist must be a string.");
        if (row->contains("artists") && !(*row)["artists"].is_array())
            throw Error("Whitelist artists must be an array.");
        if (auto url = (*row)["url"].value<std::string>()) {
            if (row->size() != 1)
                throw Error("URL whitelist entries cannot contain artist/title fields.");
            urls_.insert(song_url(*url));
        } else {
            auto title = (*row)["title"].value<std::string>();
            if (!title)
                throw Error("Whitelist entry requires URL or title and artist.");
            std::vector<std::string> artists;
            if (auto artist = (*row)["artist"].value<std::string>())
                artists.push_back(*artist);
            if (auto *array = (*row)["artists"].as_array()) {
                if (!artists.empty())
                    throw Error("Use artist or artists, not both.");
                for (const auto &name : *array) {
                    auto value = name.value<std::string>();
                    if (!value)
                        throw Error("Whitelist artists must be strings.");
                    artists.push_back(*value);
                }
            }
            identities_.insert(identity(*title, artists).key);
        }
    }
}
bool Whitelist::contains(const Song &song) const {
    return urls_.contains(song.url) || identities_.contains(identity(song.title, song.artists).key);
}
bool Whitelist::contains(const Song &song, const Identity &normalized) const {
    return urls_.contains(song.url) || identities_.contains(normalized.key);
}
}
