#include "rym_crosscheck/matching/normalize.hpp"
#include "rym_crosscheck/domain/runtime.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <regex>
#include <utf8proc.h>
#include <utility>

namespace rym {
namespace {
bool whitespace(char32_t c) noexcept {
    return c == U' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f ||
           c == 0x205f || c == 0x3000;
}
std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && value.front() == ' ')
        value.remove_prefix(1);
    while (!value.empty() && value.back() == ' ')
        value.remove_suffix(1);
    return value;
}
bool quoted(std::string_view value) noexcept {
    return value.size() > 2 && (value.front() == '"' || value.front() == '\'') &&
           value.find(value.front(), 1) == value.size() - 1;
}
std::pair<std::string_view, std::string_view> edition_suffix(std::string_view value) noexcept {
    std::size_t start = value.npos;
    std::string_view label;
    if (value.ends_with(')') || value.ends_with(']')) {
        start = value.rfind(value.back() == ')' ? '(' : '[');
        if (start != value.npos)
            label = trim(value.substr(start + 1, value.size() - start - 2));
    } else if ((start = value.rfind(" - ")) != value.npos)
        label = trim(value.substr(start + 3));
    if (label == "single version" || label == "album version")
        return {trim(value.substr(0, start)), label};
    return {value, {}};
}
std::string_view title_text(std::string_view value) {
    static const std::regex remaster(
        R"(\s*(?:\((?:[0-9]{4} )?remaster(?:ed)?(?: [0-9]{4})?\)|\[(?:[0-9]{4} )?remaster(?:ed)?(?: [0-9]{4})?\]| - (?:[0-9]{4} )?remaster(?:ed)?(?: [0-9]{4})?)$)");
    for (;;) {
        value = trim(value);
        if (quoted(value)) {
            value = value.substr(1, value.size() - 2);
            continue;
        }
        if (value.ends_with(')') || value.ends_with(']')) {
            auto start = value.rfind(value.back() == ')' ? '(' : '[');
            if (start != value.npos && start > 0) {
                auto annotation = trim(value.substr(start + 1, value.size() - start - 2));
                constexpr std::string_view prefix = "including ";
                if (annotation.starts_with(prefix) &&
                    quoted(trim(annotation.substr(prefix.size())))) {
                    value = value.substr(0, start);
                    continue;
                }
            }
        }
        std::match_results<std::string_view::const_iterator> match;
        if (std::regex_search(value.begin(), value.end(), match, remaster)) {
            value = value.substr(0, static_cast<std::size_t>(match.position()));
            continue;
        }
        return value;
    }
}
std::string version_of(std::string_view value) {
    static const std::regex qualifier(
        R"((?:\(|\[| - )(live|remix|[^\)\]]* remix|[^\)\]]* mix|acoustic|instrumental|demo|re-recorded|rerecorded|edit)(?:[^\)\]]*)(?:\)|\]|$))",
        std::regex::icase);
    std::match_results<std::string_view::const_iterator> match;
    if (std::regex_search(value.begin(), value.end(), match, qualifier))
        return normalize(value.substr(static_cast<std::size_t>(match.position()), match.length()))
            .text;
    return {};
}
}
Name normalize(std::string_view value, bool title) {
    if (value.size() > 65536)
        throw Error("Metadata field exceeds 64 KiB.");
    utf8proc_uint8_t *raw{};
    auto size = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t *>(value.data()),
                             static_cast<utf8proc_ssize_t>(value.size()), &raw,
                             static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE |
                                                            UTF8PROC_COMPAT | UTF8PROC_CASEFOLD));
    std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> buffer(raw, &std::free);
    if (size < 0)
        throw Error("Invalid Unicode metadata.");
    Name result;
    bool space = false;
    for (utf8proc_ssize_t offset = 0; offset < size;) {
        utf8proc_int32_t point{};
        auto length = utf8proc_iterate(raw + offset, size - offset, &point);
        if (length <= 0)
            throw Error("Invalid Unicode metadata.");
        offset += length;
        auto c = static_cast<char32_t>(point);
        if (whitespace(c)) {
            space = !result.points.empty();
            continue;
        }
        if (space) {
            result.points.push_back(U' ');
            space = false;
        }
        if (c == 0x2018 || c == 0x2019 || c == 0x02bc)
            c = U'\'';
        if (c == 0x201c || c == 0x201d)
            c = U'"';
        if (c >= 0x2010 && c <= 0x2015)
            c = U'-';
        if (c < 0x20)
            throw Error("Control character in metadata.");
        result.points.push_back(c);
    }
    for (auto c : result.points) {
        utf8proc_uint8_t encoded[4];
        auto length = utf8proc_encode_char(static_cast<utf8proc_int32_t>(c), encoded);
        result.text.append(reinterpret_cast<char *>(encoded), static_cast<std::size_t>(length));
    }
    if (title) {
        auto text = title_text(result.text);
        if (text.find(" /") != text.npos || text.find("/ ") != text.npos) {
            std::string compact;
            compact.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '/') {
                    if (!compact.empty() && compact.back() == ' ')
                        compact.pop_back();
                    compact += '/';
                    while (i + 1 < text.size() && text[i + 1] == ' ')
                        ++i;
                } else
                    compact += text[i];
            }
            return normalize(compact);
        }
        if (text.size() != result.text.size())
            return normalize(text);
    }
    return result;
}
Identity identity(std::string_view title, const std::vector<std::string> &artists) {
    Identity result;
    result.title = normalize(title, true);
    auto [base, edition] = edition_suffix(result.title.text);
    if (!edition.empty()) {
        result.version = edition;
        result.title = normalize(base, true);
        if (auto recording = version_of(result.title.text); !recording.empty())
            result.version = recording + " | " + result.version;
    } else
        result.version = version_of(result.title.text);
    for (const auto &artist : artists) {
        auto name = normalize(artist).text;
        if (!name.empty())
            result.artists.push_back(std::move(name));
    }
    if (result.title.text.empty() || result.artists.empty())
        throw Error("Song requires title and artist.");
    result.principal = result.artists.front();
    std::ranges::sort(result.artists);
    auto unique = std::ranges::unique(result.artists);
    result.artists.erase(unique.begin(), unique.end());
    std::string credit;
    for (const auto &artist : result.artists) {
        if (!credit.empty())
            credit += " | ";
        credit += artist;
    }
    result.credit = normalize(credit);
    result.key = std::to_string(result.title.text.size()) + ":" + result.title.text;
    for (const auto &artist : result.artists)
        result.key += std::to_string(artist.size()) + ":" + artist;
    result.key += ":" + result.version;
    return result;
}
bool review_versions(std::string_view left, std::string_view right) noexcept {
    auto edition = [](std::string_view value) noexcept {
        return value.empty() || value == "single version" || value == "album version";
    };
    return left != right && edition(left) && edition(right);
}
Fraction threshold(std::string_view value) {
    if (value.empty() || value.size() > 8)
        throw Error("Similarity thresholds must be decimals between 0 and 1 with at most six "
                    "fractional digits.");
    auto dot = value.find('.');
    std::string digits(value);
    std::uint32_t denominator = 1;
    if (dot != value.npos) {
        auto places = value.size() - dot - 1;
        if (places > 6 || places == 0)
            throw Error("Invalid similarity threshold.");
        for (std::size_t i = 0; i < places; ++i)
            denominator *= 10;
        digits.erase(dot, 1);
    }
    std::uint32_t numerator{};
    auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), numerator);
    if (error != std::errc{} || end != digits.data() + digits.size() || numerator > denominator)
        throw Error("Similarity threshold must be between 0 and 1.");
    auto divisor = std::gcd(numerator, denominator);
    return {numerator / divisor, denominator / divisor};
}
std::size_t edit_distance(std::u32string_view a, std::u32string_view b, std::size_t limit) {
    if (a.size() > b.size())
        std::swap(a, b);
    if (b.size() - a.size() > limit)
        return limit + 1;
    while (!a.empty() && !b.empty() && a.front() == b.front()) {
        a.remove_prefix(1);
        b.remove_prefix(1);
    }
    while (!a.empty() && !b.empty() && a.back() == b.back()) {
        a.remove_suffix(1);
        b.remove_suffix(1);
    }
    if (a.empty())
        return b.size();
    thread_local std::vector<std::size_t> previous, current;
    previous.assign(a.size() + 1, limit + 1);
    current.resize(a.size() + 1);
    for (std::size_t j = 0; j <= std::min(a.size(), limit); ++j)
        previous[j] = j;
    for (std::size_t i = 1; i <= b.size(); ++i) {
        if ((i & 63) == 0)
            check_cancelled();
        std::fill(current.begin(), current.end(), limit + 1);
        if (i <= limit)
            current[0] = i;
        auto begin = i > limit ? i - limit : 1;
        auto end = std::min(a.size(), i + limit);
        auto minimum = current[0];
        for (auto j = begin; j <= end; ++j) {
            current[j] = std::min(
                {current[j - 1] + 1, previous[j] + 1, previous[j - 1] + (a[j - 1] != b[i - 1])});
            minimum = std::min(minimum, current[j]);
        }
        if (minimum > limit)
            return limit + 1;
        previous.swap(current);
    }
    return previous[a.size()];
}
Fraction similarity(std::u32string_view a, std::u32string_view b, Fraction cutoff) {
    auto length = std::max(a.size(), b.size());
    if (!length)
        return {1, 1};
    auto limit = (static_cast<std::uint64_t>(cutoff.denominator - cutoff.numerator) * length) /
                 cutoff.denominator;
    auto distance = edit_distance(a, b, static_cast<std::size_t>(limit));
    if (distance > limit)
        return {0, 1};
    return {static_cast<std::uint32_t>(length - distance), static_cast<std::uint32_t>(length)};
}
bool passes(Fraction score, Fraction cutoff) noexcept {
    return std::uint64_t(score.numerator) * cutoff.denominator >=
           std::uint64_t(cutoff.numerator) * score.denominator;
}
}
