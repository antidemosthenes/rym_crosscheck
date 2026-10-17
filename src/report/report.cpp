#include "rym_crosscheck/report/report.hpp"
#include "rym_crosscheck/matching/index.hpp"
#include "rym_crosscheck/rym/parser.hpp"
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <objbase.h>

namespace rym {
namespace {
boost::json::array strings(const std::vector<std::string> &values) {
    boost::json::array result;
    result.reserve(values.size());
    for (const auto &value : values)
        result.emplace_back(value);
    return result;
}
std::string text(const boost::json::value &value) {
    const auto &str = value.as_string();
    std::string result(str.data(), str.size());
    if (result.find('\0') != result.npos)
        throw Error("Report text contains a null character.");
    wide(result);
    return result;
}
std::vector<std::string> string_list(const boost::json::value &value) {
    std::vector<std::string> result;
    result.reserve(value.as_array().size());
    for (const auto &item : value.as_array())
        result.push_back(text(item));
    return result;
}
boost::json::object score(Fraction value) {
    return {{"numerator", value.numerator}, {"denominator", value.denominator}};
}
Fraction parse_score(const boost::json::value &value) {
    const auto &object = value.as_object();
    Fraction result{object.at("numerator").to_number<std::uint32_t>(),
                    object.at("denominator").to_number<std::uint32_t>()};
    if (!result.denominator || result.numerator > result.denominator)
        throw Error("Invalid report similarity score.");
    return result;
}
void relative_path(std::string_view value) {
    auto path = std::filesystem::path(wide(value));
    if (path.empty() || path.has_root_path())
        throw Error("Candidate path must be relative.");
    for (const auto &component : path)
        if (component == "..")
            throw Error("Candidate path leaves the music directory.");
}
std::string new_generation() {
    GUID value{};
    if (FAILED(CoCreateGuid(&value)))
        throw Error("Cannot create a report generation ID.");
    wchar_t text[40]{};
    StringFromGUID2(value, text, 40);
    return utf8(text);
}
}
Report compare_library(const Snapshot &snapshot, const Scan &scan,
                       const std::filesystem::path &root, bool offline, Fraction artist_cutoff,
                       Fraction title_cutoff) {
    Report result;
    result.generation = new_generation();
    result.generated = now_seconds();
    result.captured = snapshot.captured;
    result.source = snapshot.source;
    result.offline = offline;
    result.identified = scan.tracks.size();
    result.reused = scan.reused;
    result.warnings = scan.warnings;
    result.songs.reserve(snapshot.songs.size());
    Index index(scan.tracks);
    auto absolute_root = std::filesystem::absolute(root).lexically_normal();
    for (const auto &song : snapshot.songs) {
        check_cancelled();
        auto normalized = identity(song.title, song.artists);
        ReportSong row{song, index.owns(normalized), {}};
        if (!row.owned) {
            for (const auto &candidate : index.suggest(normalized, artist_cutoff, title_cutoff)) {
                const auto &track = scan.tracks[candidate.track_id];
                auto path = track.path.is_absolute() ? track.path : absolute_root / track.path;
                auto relative = path.lexically_normal().lexically_relative(absolute_root);
                auto display = path_text(relative);
                relative_path(display);
                row.candidates.push_back({track.title, track.artists, std::move(display),
                                          candidate.artist, candidate.title, candidate.credit_gap,
                                          candidate.version_gap});
            }
        }
        result.songs.push_back(std::move(row));
    }
    return result;
}
namespace {
boost::json::array whitelist_rows(const std::vector<WhitelistEntry> &entries) {
    boost::json::array result;
    result.reserve(entries.size());
    for (const auto &entry : entries)
        result.emplace_back(boost::json::object{{"key", entry.key},
                                                {"url", entry.url},
                                                {"title", entry.title},
                                                {"artists", strings(entry.artists)}});
    return result;
}
boost::json::object report_fields(const Report &report) {
    boost::json::array songs;
    songs.reserve(report.songs.size());
    for (const auto &row : report.songs) {
        boost::json::array candidates;
        for (const auto &candidate : row.candidates)
            candidates.emplace_back(
                boost::json::object{{"title", candidate.title},
                                    {"artists", strings(candidate.artists)},
                                    {"path", candidate.path},
                                    {"artist", score(candidate.artist)},
                                    {"title_score", score(candidate.title_score)},
                                    {"credit_gap", candidate.credit_gap},
                                    {"version_gap", candidate.version_gap}});
        songs.emplace_back(boost::json::object{{"rank", row.song.rank},
                                               {"title", row.song.title},
                                               {"artists", strings(row.song.artists)},
                                               {"album", row.song.album},
                                               {"url", row.song.url},
                                               {"owned", row.owned},
                                               {"candidates", std::move(candidates)}});
    }
    return {{"schema_version", 1},
            {"generation", report.generation},
            {"generated", report.generated},
            {"source", report.source},
            {"captured", report.captured},
            {"offline", report.offline},
            {"identified", report.identified},
            {"reused", report.reused},
            {"warnings", strings(report.warnings)},
            {"songs", std::move(songs)}};
}
}
boost::json::object report_json(const Report &report) {
    auto result = report_fields(report);
    result["whitelist"] = whitelist_rows(report.whitelist);
    return result;
}
Report parse_report(const boost::json::value &json) {
    const auto &object = json.as_object();
    if (object.at("schema_version").to_number<int>() != 1)
        throw Error("Unsupported report version; run a new comparison.");
    Report result;
    result.generation = text(object.at("generation"));
    if (result.generation.empty() || result.generation.size() > 128)
        throw Error("Invalid report generation ID.");
    result.generated = object.at("generated").to_number<std::int64_t>();
    result.captured = object.at("captured").to_number<std::int64_t>();
    result.source = site_url(text(object.at("source")));
    if (result.source != "https://rateyourmusic.com/charts/top/song/all-time/" ||
        result.generated < 0 || result.captured < 0 || result.generated > 253402300799LL ||
        result.captured > 253402300799LL)
        throw Error("Invalid report source/date.");
    result.offline = object.at("offline").as_bool();
    result.identified = object.at("identified").to_number<std::size_t>();
    result.reused = object.at("reused").to_number<std::size_t>();
    if (result.reused > result.identified)
        throw Error("Invalid cached file count.");
    result.warnings = string_list(object.at("warnings"));
    boost::unordered_flat_set<std::string> urls;
    urls.reserve(object.at("songs").as_array().size());
    result.songs.reserve(object.at("songs").as_array().size());
    for (const auto &item : object.at("songs").as_array()) {
        const auto &row = item.as_object();
        ReportSong record;
        record.song = {row.at("rank").to_number<std::size_t>(), text(row.at("title")),
                       string_list(row.at("artists")), text(row.at("album")),
                       song_url(text(row.at("url")))};
        if (record.song.rank != result.songs.size() + 1 || !urls.insert(record.song.url).second)
            throw Error("Invalid report chart ranks or duplicate songs.");
        identity(record.song.title, record.song.artists);
        record.owned = row.at("owned").as_bool();
        const auto &candidates = row.at("candidates").as_array();
        if (candidates.size() > 3 || (record.owned && !candidates.empty()))
            throw Error("Invalid report candidates.");
        for (const auto &value : candidates) {
            const auto &candidate = value.as_object();
            ReportCandidate match{text(candidate.at("title")),
                                  string_list(candidate.at("artists")),
                                  text(candidate.at("path")),
                                  parse_score(candidate.at("artist")),
                                  parse_score(candidate.at("title_score")),
                                  candidate.at("credit_gap").as_bool()};
            if (const auto *gap = candidate.if_contains("version_gap"))
                match.version_gap = gap->as_bool();
            identity(match.title, match.artists);
            relative_path(match.path);
            record.candidates.push_back(std::move(match));
        }
        result.songs.push_back(std::move(record));
    }
    if (result.songs.empty())
        throw Error("Report contains no chart songs.");
    std::vector<WhitelistEntry> entries;
    entries.reserve(object.at("whitelist").as_array().size());
    for (const auto &value : object.at("whitelist").as_array()) {
        const auto &entry = value.as_object();
        entries.push_back(
            {{}, text(entry.at("url")), text(entry.at("title")), string_list(entry.at("artists"))});
    }
    result.whitelist = Whitelist(std::move(entries)).entries();
    return result;
}
boost::json::object review_json(const ReviewDocument &document) {
    auto result = document.report ? report_fields(*document.report) : boost::json::object{};
    result["empty"] = !document.report;
    result["whitelist_valid"] = document.whitelist_valid;
    result["whitelist"] = whitelist_rows(document.whitelist.entries());
    boost::unordered_flat_map<std::string, const Song *> by_url, by_identity;
    auto identities = std::ranges::any_of(document.whitelist.entries(),
                                          [](const auto &entry) { return entry.url.empty(); });
    if (document.report) {
        if (!document.whitelist.entries().empty())
            by_url.reserve(document.report->songs.size());
        if (identities)
            by_identity.reserve(document.report->songs.size());
        auto &rows = result.at("songs").as_array();
        for (std::size_t i = 0; i < document.report->songs.size(); ++i) {
            const auto &song = document.report->songs[i].song;
            if (identities) {
                auto normalized = identity(song.title, song.artists);
                rows[i].as_object()["whitelisted"] = document.whitelist.contains(song, normalized);
                by_identity.emplace(std::move(normalized.key), &song);
            } else
                rows[i].as_object()["whitelisted"] = document.whitelist.contains(song);
            if (!document.whitelist.entries().empty())
                by_url.emplace(song.url, &song);
        }
    } else
        result["songs"] = boost::json::array{};
    auto &rows = result.at("whitelist").as_array();
    for (std::size_t i = 0; i < document.whitelist.entries().size(); ++i) {
        const auto &entry = document.whitelist.entries()[i];
        const Song *known{};
        if (!entry.url.empty()) {
            if (auto found = by_url.find(entry.url); found != by_url.end())
                known = found->second;
        } else {
            if (auto found = by_identity.find(entry.key.substr(9)); found != by_identity.end())
                known = found->second;
        }
        auto &row = rows[i].as_object();
        row["display_title"] = known ? known->title : entry.title;
        row["display_artists"] = strings(known ? known->artists : entry.artists);
        row["display_album"] = known ? known->album : "";
        row["display_url"] = known ? known->url : entry.url;
    }
    return result;
}
}
