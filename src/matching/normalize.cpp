#include "rym_crosscheck/matching/normalize.hpp"
#include "rym_crosscheck/domain/runtime.hpp"
#include <algorithm>
#include <boost/container/small_vector.hpp>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <ranges>
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
std::string format_title(std::string_view value) {
    value = trim(value);
    while (quoted(value))
        value = trim(value.substr(1, value.size() - 2));
    std::string result;
    result.reserve(value.size());
    bool space{};
    for (char point : value) {
        if (point == ' ') {
            space = !result.empty();
            continue;
        }
        if (space && point != '/' && result.back() != '/')
            result += ' ';
        space = false;
        result += point;
    }
    return result;
}
bool quoted_sections(std::string_view value, std::string *sections = nullptr) {
    auto word = [](unsigned char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80;
    };
    for (;;) {
        value = trim(value);
        if (value.size() < 3 || (value.front() != '\'' && value.front() != '"'))
            return false;
        auto quote = value.front();
        auto end = value.find(quote, 1);
        while (end != value.npos && quote == '\'' && end + 1 < value.size() &&
               word(static_cast<unsigned char>(value[end - 1])) &&
               word(static_cast<unsigned char>(value[end + 1])))
            end = value.find(quote, end + 1);
        if (end == value.npos || trim(value.substr(1, end - 1)).empty())
            return false;
        if (sections) {
            auto name = trim(value.substr(1, end - 1));
            sections->append(std::to_string(name.size())).append(":").append(name);
        }
        value = trim(value.substr(end + 1));
        if (value.empty())
            return true;
        if (value.starts_with(',')) {
            value = trim(value.substr(1));
            if (value.starts_with("and "))
                value.remove_prefix(4);
            else if (value.starts_with("& "))
                value.remove_prefix(2);
        } else if (value.starts_with("and "))
            value.remove_prefix(4);
        else if (value.starts_with("& "))
            value.remove_prefix(2);
        else
            return false;
    }
}
std::pair<std::string_view, std::string_view> suffix(std::string_view value) noexcept {
    std::size_t start = value.npos;
    std::string_view label;
    if (value.ends_with(')') || value.ends_with(']')) {
        std::size_t depth = 1;
        for (std::size_t i = value.size() - 1; i-- > 0;) {
            auto point = value[i];
            if (point == ')' || point == ']')
                ++depth;
            else if ((point == '(' || point == '[') && --depth == 0) {
                if (point == (value.back() == ')' ? '(' : '['))
                    start = i;
                break;
            }
        }
        if (start != value.npos)
            label = trim(value.substr(start + 1, value.size() - start - 2));
    } else if ((start = value.rfind(" - ")) != value.npos)
        label = trim(value.substr(start + 3));
    if (start != value.npos && !trim(value.substr(0, start)).empty())
        return {trim(value.substr(0, start)), label};
    return {value, {}};
}
bool edition_label(std::string_view label) noexcept {
    if (label.empty())
        return false;
    constexpr std::string_view labels[]{"single version",
                                        "album version",
                                        "extended version",
                                        "extended mix",
                                        "extended",
                                        "radio edit",
                                        "radio version",
                                        "radio mix",
                                        "single edit",
                                        "single mix",
                                        "album edit",
                                        "album mix",
                                        "edit",
                                        "edited version",
                                        "extended edit",
                                        "short version",
                                        "short edit",
                                        "long version",
                                        "long edit",
                                        "full length version",
                                        "full-length version",
                                        "full length",
                                        "full-length",
                                        "original version",
                                        "original mix",
                                        "12\" version",
                                        "12-inch version",
                                        "7\" version",
                                        "7-inch version",
                                        "12\" mix",
                                        "7\" mix",
                                        "mono version",
                                        "stereo version",
                                        "mono",
                                        "stereo"};
    return std::ranges::find(labels, label) != std::end(labels);
}
bool remaster_label(std::string_view label) noexcept {
    auto year = [](std::string_view value) noexcept {
        return value.size() == 4 &&
               std::ranges::all_of(value, [](char c) noexcept { return c >= '0' && c <= '9'; });
    };
    if (label.size() > 5 && label[4] == ' ' && year(label.substr(0, 4)))
        label.remove_prefix(5);
    if (label.size() > 5 && label[label.size() - 5] == ' ' && year(label.substr(label.size() - 4)))
        label.remove_suffix(5);
    return label == "remaster" || label == "remastered";
}
std::string_view title_text(std::string_view value, std::string *sections) {
    for (;;) {
        value = trim(value);
        if (quoted(value)) {
            value = value.substr(1, value.size() - 2);
            continue;
        }
        auto [base, label] = suffix(value);
        constexpr std::string_view prefix = "including ";
        std::string names;
        if ((value.ends_with(')') || value.ends_with(']')) && label.starts_with(prefix) &&
            quoted_sections(label.substr(prefix.size()), sections ? &names : nullptr)) {
            if (sections)
                sections->insert(0, names);
            value = base;
            continue;
        }
        if (remaster_label(label)) {
            value = base;
            continue;
        }
        return value;
    }
}
std::string version_of(std::string_view value) {
    if (value.find_first_of("([") == value.npos && value.find(" - ") == value.npos)
        return {};
    auto word_character = [](std::string_view text) noexcept {
        auto point = static_cast<unsigned char>(text.front());
        if (point < 0x80)
            return (point >= 'a' && point <= 'z') || (point >= '0' && point <= '9') || point == '_';
        utf8proc_int32_t unicode{};
        if (utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data()),
                             static_cast<utf8proc_ssize_t>(text.size()), &unicode) <= 0)
            return true;
        auto category = utf8proc_category(unicode);
        return category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_PC;
    };
    auto starts_word = [&](std::string_view text, std::string_view word) noexcept {
        return text.starts_with(word) &&
               (text.size() == word.size() || !word_character(text.substr(word.size())));
    };
    using Range = std::pair<std::size_t, std::size_t>;
    boost::container::small_vector<Range, 4> mixes, annotations;
    for (std::size_t i = 0; i < value.size(); ++i)
        if (value[i] == ' ')
            for (auto word : {std::string_view("mix"), std::string_view("remix")})
                if (starts_word(value.substr(i + 1), word))
                    mixes.emplace_back(i, i + 1 + word.size());
    auto closing = value.size();
    for (std::size_t i = value.size(); i-- > 0;) {
        auto point = value[i];
        if (point == ')' || point == ']')
            closing = i;
        else if (point == '(' || point == '[' || value.substr(i).starts_with(" - "))
            annotations.emplace_back(i, closing);
    }
    std::string result;
    std::size_t consumed{};
    for (const auto &[start, end] : annotations | std::views::reverse) {
        if (start < consumed)
            continue;
        auto begin = start + (value[start] == ' ' ? 3 : 1);
        auto label = trim(value.substr(begin, end - begin));
        bool protected_recording{};
        for (auto word : {"live", "mix", "remix", "acoustic", "instrumental", "demo", "re-recorded",
                          "rerecorded", "edit"})
            if (starts_word(label, word)) {
                protected_recording = true;
                break;
            }
        if (!protected_recording) {
            auto mix = std::ranges::lower_bound(mixes, begin, {}, &Range::first);
            protected_recording = mix != mixes.end() && mix->second <= end;
        }
        if (!protected_recording)
            continue;
        if (!result.empty())
            result += " | ";
        consumed = end < value.size() ? end + 1 : end;
        result += trim(value.substr(start, consumed - start));
    }
    return result;
}
}
std::u32string decode_utf8(std::string_view value) {
    std::u32string result;
    result.reserve(value.size());
    for (std::size_t offset = 0; offset < value.size();) {
        utf8proc_int32_t point{};
        auto length =
            utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(value.data() + offset),
                             static_cast<utf8proc_ssize_t>(value.size() - offset), &point);
        if (length <= 0)
            throw Error("Invalid Unicode metadata.");
        result.push_back(static_cast<char32_t>(point));
        offset += static_cast<std::size_t>(length);
    }
    return result;
}
std::optional<std::u32string> latin_diacritics(std::u32string_view normalized) {
    if (std::ranges::all_of(normalized, [](char32_t point) noexcept { return point < 0x80; }))
        return {};
    auto latin = [](char32_t point) noexcept {
        return (point >= U'a' && point <= U'z') || (point >= U'A' && point <= U'Z');
    };
    std::optional<std::u32string> result;
    bool previous_latin{};
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        auto point = normalized[i];
        auto replacement = point;
        bool remove{};
        if (point >= 0x80) {
            utf8proc_int32_t decomposed{};
            auto count = utf8proc_decompose_char(
                static_cast<utf8proc_int32_t>(point), &decomposed, 1,
                UTF8PROC_DECOMPOSE | UTF8PROC_STRIPMARK | UTF8PROC_STABLE, nullptr);
            if (count == 1 && latin(static_cast<char32_t>(decomposed)))
                replacement = static_cast<char32_t>(decomposed);
            else if (count == 0 && previous_latin &&
                     ((point >= 0x0300 && point <= 0x036f) ||
                      (point >= 0x1ab0 && point <= 0x1aff) ||
                      (point >= 0x1dc0 && point <= 0x1dff) ||
                      (point >= 0xfe20 && point <= 0xfe2f)))
                remove = true;
        }
        if (!result && (remove || replacement != point)) {
            result.emplace(normalized.substr(0, i));
            result->reserve(normalized.size());
        }
        if (result && !remove)
            result->push_back(replacement);
        if (!remove)
            previous_latin = latin(replacement);
    }
    return result;
}
namespace {
Name normalize_name(std::string_view value, bool title, std::string *sections,
                    AnnotationText *annotations = nullptr) {
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
    result.points.reserve(static_cast<std::size_t>(size));
    result.text.reserve(static_cast<std::size_t>(size));
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
        if (c < 0x80) {
            result.text.push_back(static_cast<char>(c));
            continue;
        }
        utf8proc_uint8_t encoded[4];
        auto length = utf8proc_encode_char(static_cast<utf8proc_int32_t>(c), encoded);
        result.text.append(reinterpret_cast<char *>(encoded), static_cast<std::size_t>(length));
    }
    if (title) {
        if (annotations)
            *annotations = extract_annotations(result.text);
        auto text = title_text(result.text, sections);
        if (text.find(" /") != text.npos || text.find("/ ") != text.npos) {
            result.text = format_title(text);
            result.points = decode_utf8(result.text);
            return result;
        }
        if (text.size() != result.text.size()) {
            result.text.assign(text);
            result.points = decode_utf8(result.text);
        }
    }
    return result;
}
}
Name normalize(std::string_view value, bool title) {
    return normalize_name(value, title, nullptr);
}
AnnotationText extract_annotations(std::string_view normalized) {
    if (normalized.find_first_of("([])") == normalized.npos &&
        normalized.find(" - ") == normalized.npos)
        return {};
    AnnotationText result;
    result.source = format_title(normalized);
    result.base.reserve(result.source.size());
    boost::container::small_vector<char, 8> brackets;
    for (char point : result.source) {
        if (point == '(' || point == '[') {
            brackets.push_back(point);
            result.annotated = true;
        } else if (point == ')' || point == ']') {
            if (brackets.empty() || brackets.back() != (point == ')' ? '(' : '['))
                return {result.source, std::move(result.source), false};
            brackets.pop_back();
            if (brackets.empty())
                result.base += ' ';
        } else if (brackets.empty())
            result.base += point;
    }
    if (!brackets.empty())
        return {result.source, std::move(result.source), false};
    result.base = format_title(result.base);
    return result;
}
namespace {
Identity make_identity(std::string_view title, const std::vector<std::string> &artists,
                       bool prepare_review) {
    Identity result;
    result.title = normalize_name(title, true, &result.sections,
                                  prepare_review ? &result.annotations : nullptr);
    std::string recordings;
    for (;;) {
        auto [base, edition] = suffix(result.title.text);
        auto is_edition = edition_label(edition);
        auto recording = !edition.empty() && !is_edition
                             ? version_of(std::string_view(result.title.text).substr(base.size()))
                             : std::string{};
        if (recording != trim(std::string_view(result.title.text).substr(base.size())))
            recording.clear();
        if (!is_edition && recording.empty())
            break;
        auto &destination = is_edition ? result.version : recordings;
        std::string versions = is_edition ? std::string(edition) : std::move(recording);
        if (!destination.empty())
            versions.append(" | ").append(destination);
        destination = std::move(versions);
        result.title = normalize_name(base, true, &result.sections);
    }
    if (auto recording = version_of(result.title.text); !recording.empty()) {
        if (!recordings.empty())
            recording.append(" | ").append(recordings);
        recordings = std::move(recording);
    }
    if (!recordings.empty()) {
        if (!result.version.empty())
            recordings.append(" | ").append(result.version);
        result.version = std::move(recordings);
    }
    result.artists.reserve(artists.size());
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
    std::size_t credit_size = (result.artists.size() - 1) * 3;
    for (const auto &artist : result.artists)
        credit_size += artist.size();
    if (credit_size > 65536)
        throw Error("Metadata field exceeds 64 KiB.");
    if (prepare_review) {
        auto &credit = result.credit.text;
        credit.reserve(credit_size);
        for (const auto &artist : result.artists) {
            if (!credit.empty())
                credit += " | ";
            credit += artist;
        }
        result.credit.points = decode_utf8(credit);
    }
    auto encoded_size = [](std::string_view value) noexcept {
        auto size = value.size();
        return size + 2 + (size >= 10) + (size >= 100) + (size >= 1000) + (size >= 10000);
    };
    auto key_size = encoded_size(result.title.text) + encoded_size(result.version) +
                    encoded_size(result.sections) + 1;
    for (const auto &artist : result.artists)
        key_size += encoded_size(artist);
    result.key.reserve(key_size);
    auto append_key = [&](std::string_view value) {
        result.key.append(std::to_string(value.size())).append(":").append(value);
    };
    append_key(result.title.text);
    for (const auto &artist : result.artists)
        append_key(artist);
    result.key += ':';
    append_key(result.version);
    append_key(result.sections);
    if (!result.annotations.annotated && result.annotations.base == result.title.text)
        result.annotations = {};
    return result;
}
}
Identity identity(std::string_view title, const std::vector<std::string> &artists) {
    return make_identity(title, artists, true);
}
std::string identity_key(std::string_view title, const std::vector<std::string> &artists) {
    return make_identity(title, artists, false).key;
}
bool review_versions(std::string_view left, std::string_view right) noexcept {
    auto recording = [](std::string_view value) noexcept {
        for (;;) {
            auto separator = value.rfind(" | ");
            auto label = separator == value.npos ? value : value.substr(separator + 3);
            if (!edition_label(label))
                return value;
            if (separator == value.npos)
                return std::string_view{};
            value = value.substr(0, separator);
        }
    };
    return left != right && recording(left) == recording(right);
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
        auto begin = i > limit ? i - limit : 1;
        auto end = std::min(a.size(), i + limit);
        current[begin - 1] = begin == 1 && i <= limit ? i : limit + 1;
        if (end < a.size())
            current[end + 1] = limit + 1;
        auto minimum = current[begin - 1];
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
