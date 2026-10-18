#include "rym_crosscheck/storage/whitelist.hpp"
#include "rym_crosscheck/matching/normalize.hpp"
#include "rym_crosscheck/rym/parser.hpp"
#include "rym_crosscheck/storage/files.hpp"
#include <algorithm>
#include <sstream>
#include <toml++/toml.hpp>

namespace rym {
Whitelist::Whitelist(const std::filesystem::path &path, bool required) {
    if (!std::filesystem::exists(path)) {
        if (required)
            throw Error("Whitelist does not exist: " + path_text(path));
        return;
    }
    *this = parse(read_file(path));
}
Whitelist Whitelist::parse(std::string_view text) {
    auto table = toml::parse(text);
    for (const auto &[key, value] : table)
        if (key != "songs")
            throw Error("Unknown whitelist key: " + std::string(key.str()));
    auto *songs = table["songs"].as_array();
    if (!songs) {
        if (table.contains("songs"))
            throw Error("Whitelist songs must be an array of tables.");
        return {};
    }
    std::vector<WhitelistEntry> entries;
    for (const auto &entry : *songs) {
        const auto *row = entry.as_table();
        if (!row)
            throw Error("Each whitelist entry must be a table.");
        for (const auto &[key, value] : *row)
            if (key != "url" && key != "artist" && key != "artists" && key != "title")
                throw Error("Unknown whitelist entry key.");
        WhitelistEntry record;
        if (row->contains("url")) {
            auto url = (*row)["url"].value<std::string>();
            if (!url || row->size() != 1)
                throw Error("URL whitelist entries require only a string URL.");
            record.url = std::move(*url);
        } else {
            auto title = (*row)["title"].value<std::string>();
            if (!title)
                throw Error("Whitelist entry requires URL or title and artist.");
            record.title = std::move(*title);
            if (row->contains("artist")) {
                auto artist = (*row)["artist"].value<std::string>();
                if (!artist || row->contains("artists"))
                    throw Error("Use one string artist or an artists array, not both.");
                record.artists.push_back(std::move(*artist));
            } else {
                auto *artists = (*row)["artists"].as_array();
                if (!artists)
                    throw Error("Whitelist artists must be an array.");
                for (const auto &artist : *artists) {
                    auto name = artist.value<std::string>();
                    if (!name)
                        throw Error("Whitelist artists must be strings.");
                    record.artists.push_back(std::move(*name));
                }
            }
        }
        entries.push_back(std::move(record));
    }
    return Whitelist(std::move(entries));
}
Whitelist::Whitelist(std::vector<WhitelistEntry> entries) {
    entries_.reserve(entries.size());
    auto urls = static_cast<std::size_t>(
        std::ranges::count_if(entries, [](const auto &entry) { return !entry.url.empty(); }));
    urls_.reserve(urls);
    identities_.reserve(entries.size() - urls);
    for (auto &entry : entries) {
        bool inserted{};
        if (!entry.url.empty()) {
            if (!entry.title.empty() || !entry.artists.empty())
                throw Error("URL whitelist entries cannot contain artist/title fields.");
            entry.url = song_url(entry.url);
            entry.key = "url:" + entry.url;
            inserted = urls_.insert(entry.url).second;
        } else {
            auto key = identity_key(entry.title, entry.artists);
            entry.key = "identity:" + key;
            inserted = identities_.insert(std::move(key)).second;
        }
        if (inserted)
            entries_.push_back(std::move(entry));
    }
}
std::string Whitelist::toml() const {
    toml::array songs;
    for (const auto &entry : entries_) {
        toml::table song;
        if (!entry.url.empty())
            song.insert("url", entry.url);
        else {
            song.insert("title", entry.title);
            toml::array artists;
            for (const auto &artist : entry.artists)
                artists.push_back(artist);
            song.insert("artists", std::move(artists));
        }
        songs.push_back(std::move(song));
    }
    toml::table table;
    table.insert("songs", std::move(songs));
    std::ostringstream result;
    result << toml::toml_formatter(table) << '\n';
    return result.str();
}
bool Whitelist::contains(const Song &song) const {
    return urls_.contains(song.url) ||
           (!identities_.empty() && identities_.contains(identity_key(song.title, song.artists)));
}
bool Whitelist::contains(const Song &song, const Identity &normalized) const {
    return urls_.contains(song.url) || identities_.contains(normalized.key);
}
}
