#include "rym_crosscheck/report/markdown.hpp"
#include "rym_crosscheck/domain/runtime.hpp"
#include <ctime>
#include <iomanip>
#include <sstream>

namespace rym {
namespace {
std::string escape(std::string_view text) {
    std::string result;
    for (char c : text) {
        if (c == '\n' || c == '\r') {
            result += " ";
            continue;
        }
        if (c == '&') {
            result += "&amp;";
            continue;
        }
        if (c == '<') {
            result += "&lt;";
            continue;
        }
        if (c == '>') {
            result += "&gt;";
            continue;
        }
        if (c == '|' || c == '[' || c == ']' || c == '\\' || c == '*' || c == '_' || c == '`')
            result += '\\';
        result += c;
    }
    return result;
}
std::string artists(const std::vector<std::string> &values) {
    std::string result;
    for (const auto &v : values) {
        if (!result.empty())
            result += ", ";
        result += v;
    }
    return result;
}
}
std::string markdown(const Snapshot &snapshot, const Scan &scan,
                     const std::vector<Missing> &missing, const std::filesystem::path &root,
                     bool offline) {
    std::ostringstream result;
    result << "# Missing RYM songs\n\n";
    result << "Compared top " << snapshot.songs.size() << " songs with " << scan.tracks.size()
           << " identified local files. " << missing.size() << " songs require attention.\n\n";
    std::time_t captured = static_cast<std::time_t>(snapshot.captured);
    std::tm utc{};
    if (gmtime_s(&utc, &captured))
        throw Error("Invalid snapshot date.");
    result << "Chart: [RYM all-time song chart](" << snapshot.source << "). Captured "
           << std::put_time(&utc, "%Y-%m-%d %H:%M:%S UTC") << ".";
    if (offline)
        result << " **Offline snapshot**, age "
               << std::max<std::int64_t>(0, now_seconds() - snapshot.captured) / 3600 << " hours.";
    result << "\n\n";
    if (!scan.warnings.empty())
        result << "**Local scan incomplete:** " << scan.warnings.size()
               << " files/directories could not be identified. Some missing rows may correspond to "
                  "these files.\n\n";
    result << "| Song | Artist | Album | Status | Possible local matches |\n| --- | --- | --- | "
              "--- | --- |\n";
    for (const auto &row : missing) {
        result << "| [" << escape(row.song.title) << "](<" << row.song.url << ">) | "
               << escape(artists(row.song.artists)) << " | "
               << escape(row.song.album.empty() ? "Unknown" : row.song.album) << " | "
               << (row.candidates.empty() ? "Missing" : "Needs review") << " | ";
        bool first = true;
        for (const auto &candidate : row.candidates) {
            if (!first)
                result << "<br>";
            first = false;
            const auto &track = scan.tracks[candidate.track_id];
            auto relative = track.path.lexically_relative(std::filesystem::absolute(root));
            result << escape(path_text(relative.empty() ? track.path : relative)) << " — ";
            result << std::fixed << std::setprecision(1);
            if (candidate.credit_gap)
                result << "Credit mismatch; ";
            else
                result << "artist " << 100 * candidate.artist.value() << "%, ";
            result << "title " << 100 * candidate.title.value() << "%";
        }
        result << " |\n";
    }
    if (missing.empty())
        result << "\nAll compared songs are owned or whitelisted.\n";
    if (!scan.warnings.empty()) {
        result << "\n## Unresolved local files\n\n";
        for (const auto &warning : scan.warnings)
            result << "- " << escape(warning) << '\n';
    }
    return result.str();
}
}
