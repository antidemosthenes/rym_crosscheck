#include "rym_crosscheck/matching/index.hpp"
#include "rym_crosscheck/domain/runtime.hpp"
#include <algorithm>
#include <array>
#include <boost/container/small_vector.hpp>
#include <boost/container_hash/hash.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <intrin.h>
#include <limits>
#include <numeric>
#include <utf8proc.h>

namespace rym {
namespace {
using Gram = std::array<char32_t, 3>;
struct GramHash {
    std::size_t operator()(const Gram &gram) const noexcept {
        auto value = std::size_t(1469598103934665603ULL);
        for (auto c : gram)
            value = (value ^ c) * 1099511628211ULL;
        return value;
    }
};
using Counts = boost::unordered_flat_map<Gram, std::size_t, GramHash>;
Counts grams(std::u32string_view text) {
    Counts result;
    result.reserve(text.size());
    for (std::size_t i = 2; i < text.size(); ++i)
        ++result[{text[i - 2], text[i - 1], text[i]}];
    return result;
}
struct Posting {
    std::size_t id;
    std::size_t count;
};
struct CodepointHash {
    using is_transparent = void;
    std::size_t operator()(std::u32string_view text) const noexcept {
        return boost::hash<std::u32string_view>{}(text);
    }
};
using TitleParts = boost::container::small_vector<std::u32string_view, 2>;
TitleParts title_parts(std::u32string_view title) {
    TitleParts result;
    if (title.find(U'/') == title.npos)
        return result;
    boost::container::small_vector<char32_t, 8> brackets;
    std::size_t start{};
    auto append = [&](std::size_t end) {
        auto part = title.substr(start, end - start);
        while (!part.empty() && part.front() == U' ')
            part.remove_prefix(1);
        while (!part.empty() && part.back() == U' ')
            part.remove_suffix(1);
        while (part.size() > 2 && (part.front() == U'"' || part.front() == U'\'') &&
               part.find(part.front(), 1) == part.size() - 1) {
            part = part.substr(1, part.size() - 2);
            while (!part.empty() && part.front() == U' ')
                part.remove_prefix(1);
            while (!part.empty() && part.back() == U' ')
                part.remove_suffix(1);
        }
        if (part.empty())
            return false;
        if (part.size() >= 3 && std::ranges::any_of(part, [](char32_t point) noexcept {
                auto category = utf8proc_category(static_cast<utf8proc_int32_t>(point));
                return category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_LO;
            }))
            result.push_back(part);
        return true;
    };
    bool split{};
    for (std::size_t i = 0; i < title.size(); ++i) {
        auto point = title[i];
        if (point == U'(' || point == U'[')
            brackets.push_back(point);
        else if (point == U')' || point == U']') {
            if (brackets.empty() || brackets.back() != (point == U')' ? U'(' : U'['))
                return {};
            brackets.pop_back();
        } else if (point == U'/' && brackets.empty()) {
            if (!append(i))
                return {};
            start = i + 1;
            split = true;
        }
    }
    if (!split || !brackets.empty() || !append(title.size()))
        return {};
    return result;
}
bool compound_title(std::u32string_view query, std::u32string_view local,
                    const std::vector<std::u32string_view> &query_parts) {
    if (query == local)
        return false;
    if (std::ranges::find(query_parts, local) != query_parts.end())
        return true;
    auto local_parts = title_parts(local);
    return std::ranges::find(local_parts, query) != local_parts.end();
}
std::string utf8_text(std::u32string_view points) {
    std::string result;
    result.reserve(points.size());
    for (auto point : points) {
        utf8proc_uint8_t encoded[4];
        auto length = utf8proc_encode_char(static_cast<utf8proc_int32_t>(point), encoded);
        result.append(reinterpret_cast<const char *>(encoded), static_cast<std::size_t>(length));
    }
    return result;
}
std::optional<std::u32string> latin_credit(const std::vector<std::string> &artists) {
    std::optional<std::vector<std::string>> names;
    for (std::size_t i = 0; i < artists.size(); ++i) {
        if (std::ranges::all_of(artists[i], [](unsigned char point) noexcept { return point < 0x80; }))
            continue;
        auto folded = latin_diacritics(decode_utf8(artists[i]));
        if (!folded)
            continue;
        if (!names)
            names = artists;
        (*names)[i] = utf8_text(*folded);
    }
    if (!names)
        return {};
    std::ranges::sort(*names);
    std::u32string result;
    for (const auto &name : *names) {
        if (!result.empty())
            result += U" | ";
        result += decode_utf8(name);
    }
    return result;
}
Fraction complete_similarity(std::u32string_view left, std::u32string_view right) {
    auto length = std::max(left.size(), right.size());
    if (!length)
        return {1, 1};
    auto shorter = std::min(left.size(), right.size());
    auto bound = length - shorter;
    for (std::size_t i = 0; i < shorter; ++i)
        bound += left[i] != right[i];
    auto distance = edit_distance(left, right, bound);
    return {static_cast<std::uint32_t>(length - distance), static_cast<std::uint32_t>(length)};
}
struct CompoundQuery {
    std::u32string_view text;
    std::vector<std::u32string_view> parts;
    std::vector<std::string> part_texts;
    bool annotated{};
    CompoundQuery(std::u32string_view value, bool extracted, bool need_text)
        : text(value), annotated(extracted) {
        auto components = title_parts(value);
        parts.assign(components.begin(), components.end());
        if (need_text) {
            part_texts.reserve(parts.size());
            for (auto part : parts)
                part_texts.push_back(utf8_text(part));
        }
    }
};
struct CompoundMatch {
    Fraction score{0, 1};
    bool annotated{};
};
void retain_compound(CompoundMatch &current, std::size_t short_length,
                     std::size_t long_length, bool annotated) noexcept {
    Fraction score{static_cast<std::uint32_t>(short_length),
                   static_cast<std::uint32_t>(long_length)};
    if (!current.score.numerator || (current.annotated && !annotated) ||
        (current.annotated == annotated &&
         std::uint64_t(score.numerator) * current.score.denominator >
             std::uint64_t(current.score.numerator) * score.denominator))
        current = CompoundMatch{score, annotated};
}
std::string_view artist_body(std::string_view name) noexcept {
    for (auto article : {std::string_view("the "), std::string_view("an "), std::string_view("a ")})
        if (name.starts_with(article)) {
            name.remove_prefix(article.size());
            break;
        }
    return name;
}
template <class Function>
void artist_parts(std::string_view name, char separator,
                  Function function) noexcept(noexcept(function(name))) {
    while (!name.empty()) {
        auto end = name.find(separator);
        if (end != 0)
            function(name.substr(0, end));
        if (end == name.npos)
            break;
        name.remove_prefix(end + 1);
    }
}
std::vector<std::string> artist_pairs(std::string_view name) {
    std::vector<std::string> result;
    artist_parts(artist_body(name), '/', [&](std::string_view part) {
        std::string_view previous;
        artist_parts(part, ' ', [&](std::string_view word) {
            if (!previous.empty()) {
                std::string pair;
                pair.reserve(previous.size() + word.size() + 1);
                pair.append(previous).append(" ").append(word);
                result.push_back(std::move(pair));
            }
            previous = word;
        });
    });
    return result;
}
bool artist_overlap(std::string_view left, std::string_view right) noexcept {
    if (left == right)
        return false;
    left = artist_body(left);
    right = artist_body(right);
    if (left.size() > right.size())
        std::swap(left, right);
    std::size_t meaningful{};
    artist_parts(left, '/', [&](std::string_view part) noexcept {
        std::size_t count{};
        artist_parts(part, ' ', [&](std::string_view word) noexcept {
            if (word != "the" && word != "a" && word != "an" && word != "and" && word != "&" &&
                word != "of" && word != "or" && word != "with" && word != "feat." && word != "feat")
                ++count;
        });
        meaningful = std::max(meaningful, count);
    });
    if (meaningful < 2)
        return false;
    for (auto start = right.find(left); start != right.npos; start = right.find(left, start + 1)) {
        auto end = start + left.size();
        if ((!start || right[start - 1] == ' ' || right[start - 1] == '/') &&
            (end == right.size() || right[end] == ' ' || right[end] == '/'))
            return true;
    }
    return false;
}
class NameIndex {
    std::vector<std::size_t> lengths_;
    boost::unordered_flat_map<std::size_t, std::vector<std::size_t>> buckets_;
    boost::unordered_flat_map<Gram, std::vector<Posting>, GramHash> postings_;
    bool dense_{};

  public:
    void add(std::u32string_view name) {
        auto id = lengths_.size();
        lengths_.push_back(name.size());
        buckets_[name.size()].push_back(id);
        for (auto [gram, count] : grams(name))
            postings_[gram].push_back({id, count});
    }
    void freeze() noexcept {
        std::size_t common{};
        for (const auto &[gram, postings] : postings_)
            if (postings.size() * 4 >= lengths_.size() * 3)
                ++common;
        dense_ = lengths_.size() < 32 || common >= 4;
    }
    std::vector<std::size_t> query(std::u32string_view name, Fraction cutoff,
                                   bool exhaustive) const {
        std::vector<std::size_t> result;
        if (exhaustive) {
            result.resize(lengths_.size());
            std::iota(result.begin(), result.end(), 0);
            return result;
        }
        auto m = name.size();
        auto a = std::uint64_t(cutoff.numerator);
        auto b = std::uint64_t(cutoff.denominator);
        auto low = (a * m + b - 1) / b;
        auto high = a ? b * m / a : std::numeric_limits<std::size_t>::max();
        std::size_t compatible{};
        for (const auto &[length, ids] : buckets_)
            if (length >= low && length <= high)
                compatible += ids.size();
        auto fallback = [&] {
            result.reserve(compatible);
            for (const auto &[length, ids] : buckets_)
                if (length >= low && length <= high)
                    result.insert(result.end(), ids.begin(), ids.end());
        };
        if (dense_ || m < 3) {
            fallback();
            return result;
        }
        auto query_grams = grams(name);
        std::size_t posting_work{};
        for (const auto &[gram, count] : query_grams)
            if (auto found = postings_.find(gram); found != postings_.end())
                posting_work += found->second.size();
        if (posting_work > compatible * 2) {
            fallback();
            return result;
        }
        boost::unordered_flat_map<std::size_t, std::size_t> overlap;
        for (auto [gram, count] : query_grams) {
            auto found = postings_.find(gram);
            if (found == postings_.end())
                continue;
            for (auto posting : found->second)
                if (lengths_[posting.id] >= low && lengths_[posting.id] <= high)
                    overlap[posting.id] += std::min(count, posting.count);
        }
        for (const auto &[length, ids] : buckets_) {
            if (length < low || length > high)
                continue;
            auto k = (b - a) * std::max(m, length) / b;
            auto count = std::max(m >= 3 ? m - 2 : 0, length >= 3 ? length - 2 : 0);
            if (length < 3 || count <= 3 * k)
                result.insert(result.end(), ids.begin(), ids.end());
            else {
                auto required = count - 3 * k;
                for (auto id : ids) {
                    auto found = overlap.find(id);
                    if (found != overlap.end() && found->second >= required)
                        result.push_back(id);
                }
            }
        }
        return result;
    }
};
class LatinIndex {
    boost::unordered_flat_map<std::u32string, std::vector<std::size_t>, CodepointHash,
                              std::equal_to<>> changed_;

  public:
    void add(std::size_t id, std::optional<std::u32string> spelling) {
        if (spelling)
            changed_[std::move(*spelling)].push_back(id);
    }
    std::vector<std::size_t> query(std::u32string_view original,
                                  const std::optional<std::u32string> &spelling,
                                  const NameIndex &index, const auto &get_original,
                                  const auto &fold_local, bool exhaustive) const {
        std::vector<std::size_t> result;
        auto key = spelling ? std::u32string_view(*spelling) : original;
        if (exhaustive) {
            for (auto id : index.query(original, {1, 1}, true)) {
                check_cancelled();
                auto local = get_original(id);
                if (local == original)
                    continue;
                auto folded = fold_local(id);
                if ((folded ? std::u32string_view(*folded) : local) == key)
                    result.push_back(id);
            }
            return result;
        }
        if (changed_.empty() && !spelling)
            return result;
        if (auto found = changed_.find(key); found != changed_.end())
            for (auto id : found->second)
                if (get_original(id) != original)
                    result.push_back(id);
        if (spelling)
            for (auto id : index.query(*spelling, {1, 1}, false))
                if (get_original(id) == key && get_original(id) != original)
                    result.push_back(id);
        return result;
    }
};
int compare(Fraction a, Fraction b) noexcept {
    auto left = std::uint64_t(a.numerator) * b.denominator;
    auto right = std::uint64_t(b.numerator) * a.denominator;
    return (left > right) - (left < right);
}
int compare_ratio(std::uint64_t an, std::uint64_t ad, std::uint64_t bn, std::uint64_t bd) noexcept {
    std::uint64_t left_high{}, right_high{};
    auto left = _umul128(an, bd, &left_high);
    auto right = _umul128(bn, ad, &right_high);
    if (left_high != right_high)
        return (left_high > right_high) - (left_high < right_high);
    return (left > right) - (left < right);
}
}
struct Index::Data {
    struct IndexedIdentity {
        Name title;
        std::vector<std::string> artists;
        std::string principal, version, sections;
    };
    const std::vector<Track> &tracks;
    std::vector<IndexedIdentity> identities;
    std::vector<std::size_t> representatives;
    std::vector<Name> artists;
    std::vector<std::size_t> artist_ids;
    std::vector<std::vector<std::size_t>> credit_tracks;
    boost::unordered_flat_map<std::string, std::size_t> exact;
    boost::unordered_flat_map<std::string, std::size_t> principals;
    std::vector<std::string> principal_names;
    std::vector<std::vector<std::size_t>> principal_tracks;
    boost::unordered_flat_map<std::string, std::vector<std::size_t>> principal_pairs;
    struct ComponentPosting {
        std::size_t id;
        std::uint32_t length;
        bool annotated;
    };
    boost::unordered_flat_map<std::u32string, std::vector<ComponentPosting>, CodepointHash,
                              std::equal_to<>> compound_parts;
    boost::unordered_flat_map<std::size_t, boost::container::small_vector<AnnotationText, 1>>
        annotation_variants;
    boost::unordered_flat_map<std::string, std::vector<std::size_t>> annotation_titles;
    NameIndex artist_index, title_index;
    LatinIndex latin_artists, latin_titles;
    static const std::string &base(const Identity &query) noexcept {
        return query.annotations.source.empty() ? query.title.text : query.annotations.base;
    }
    bool annotation_match(const Identity &query, std::size_t id) const {
        if (annotation_titles.empty() && !query.annotations.annotated)
            return false;
        std::string_view query_base = base(query);
        if (query_base.empty())
            return false;
        auto query_source = query.annotations.source.empty()
                                ? std::string_view(query.title.text)
                                : std::string_view(query.annotations.source);
        auto matches = [&](const AnnotationText &parts) noexcept {
            if (!parts.annotated && !query.annotations.annotated)
                return false;
            auto local_base = parts.source.empty() ? std::string_view(identities[id].title.text)
                                                   : std::string_view(parts.base);
            auto local_source = parts.source.empty() ? std::string_view(identities[id].title.text)
                                                     : std::string_view(parts.source);
            return local_base == query_base && local_source != query_source;
        };
        if (auto found = annotation_variants.find(id); found != annotation_variants.end())
            return std::ranges::any_of(found->second, matches);
        return matches({});
    }
    auto indexed_compounds(const auto &forms, const auto &eligible) const {
        boost::unordered_flat_map<std::size_t, CompoundMatch> matches;
        auto retain = [&](std::size_t id, std::size_t short_length, std::size_t long_length,
                          bool annotated) {
            if (eligible(id))
                retain_compound(matches[id], short_length, long_length, annotated);
        };
        for (const auto &form : forms) {
            if (auto found = compound_parts.find(form.text); found != compound_parts.end())
                for (const auto &posting : found->second)
                    retain(posting.id, form.text.size(), posting.length,
                           form.annotated || posting.annotated);
            for (std::size_t part = 0; part < form.parts.size(); ++part) {
                auto text = form.parts[part];
                for (auto id : title_index.query(text, {1, 1}, false))
                    if (identities[id].title.points == text)
                        retain(id, text.size(), form.text.size(), form.annotated);
                if (!form.part_texts.empty())
                    if (auto found = annotation_titles.find(form.part_texts[part]);
                        found != annotation_titles.end())
                        for (auto id : found->second)
                            retain(id, text.size(), form.text.size(), true);
            }
        }
        return matches;
    }
    CompoundMatch exhaustive_compound(const auto &forms, std::size_t id) const {
        CompoundMatch result;
        for (const auto &query : forms) {
            auto inspect = [&](std::u32string_view local, bool annotated) {
                if (compound_title(query.text, local, query.parts))
                    retain_compound(result, std::min(query.text.size(), local.size()),
                                    std::max(query.text.size(), local.size()),
                                    query.annotated || annotated);
            };
            inspect(identities[id].title.points, false);
            if (auto variants = annotation_variants.find(id); variants != annotation_variants.end())
                for (const auto &parts : variants->second)
                    if (parts.annotated && !parts.base.empty() &&
                        parts.base != identities[id].title.text &&
                        (!query.parts.empty() || parts.base.find('/') != parts.base.npos)) {
                        auto local = decode_utf8(parts.base);
                        inspect(local, true);
                    }
        }
        return result;
    }
    explicit Data(const std::vector<Track> &source) : tracks(source) {
        identities.reserve(source.size());
        representatives.reserve(source.size());
        artist_ids.reserve(source.size());
        exact.reserve(source.size());
        boost::unordered_flat_map<std::string, std::size_t> credit_ids;
        boost::unordered_flat_map<std::size_t, std::vector<std::size_t>> alternate_principals;
        auto bind_annotations = [&](AnnotationText parts, std::size_t slot, bool duplicate) {
            if (parts.source.empty()) {
                if (auto found = annotation_variants.find(slot);
                    found != annotation_variants.end() &&
                    std::ranges::none_of(found->second,
                                         [](const auto &item) { return item.source.empty(); }))
                    found->second.emplace_back();
                return;
            }
            if (parts.annotated && !parts.base.empty()) {
                auto &ids = annotation_titles[parts.base];
                if (!duplicate || std::ranges::find(ids, slot) == ids.end())
                    ids.push_back(slot);
            }
            auto [found, fresh] = annotation_variants.try_emplace(slot);
            if (fresh && duplicate)
                found->second.emplace_back();
            if (std::ranges::none_of(found->second,
                                     [&](const auto &item) { return item.source == parts.source; }))
                found->second.push_back(std::move(parts));
        };
        auto bind_principal = [&](const std::string &name, std::size_t slot, bool duplicate) {
            if (duplicate && name == identities[slot].principal)
                return;
            auto [principal, fresh] = principals.emplace(name, principal_names.size());
            if (fresh) {
                auto principal_id = principal_names.size();
                principal_names.push_back(name);
                principal_tracks.emplace_back();
                auto pairs = artist_pairs(name);
                std::ranges::sort(pairs);
                auto unique_pairs = std::ranges::unique(pairs);
                pairs.erase(unique_pairs.begin(), unique_pairs.end());
                for (auto &pair : pairs)
                    principal_pairs[std::move(pair)].push_back(principal_id);
            }
            if (duplicate) {
                auto &seen = alternate_principals[slot];
                if (std::ranges::find(seen, principal->second) != seen.end())
                    return;
                seen.push_back(principal->second);
            }
            principal_tracks[principal->second].push_back(slot);
        };
        auto bind_components = [&](std::size_t id, std::u32string_view text, bool annotated) {
            for (auto part : title_parts(text)) {
                auto &postings = compound_parts[std::u32string(part)];
                auto length = static_cast<std::uint32_t>(text.size());
                if (postings.empty() || postings.back().id != id ||
                    postings.back().length != length || postings.back().annotated != annotated)
                    postings.push_back({id, length, annotated});
            }
        };
        for (std::size_t track = 0; track < source.size(); ++track) {
            if ((track & 63) == 0)
                check_cancelled();
            auto id = identity(source[track].title, source[track].artists);
            auto [existing, inserted] = exact.emplace(std::move(id.key), identities.size());
            if (!inserted) {
                auto &representative = representatives[existing->second];
                if (source[track].path.native() < source[representative].path.native())
                    representative = track;
                bind_principal(id.principal, existing->second, true);
                bind_annotations(std::move(id.annotations), existing->second, true);
                continue;
            }
            auto slot = identities.size();
            bind_annotations(std::move(id.annotations), slot, false);
            auto [credit, fresh] = credit_ids.emplace(id.credit.text, artists.size());
            if (fresh) {
                artist_index.add(id.credit.points);
                latin_artists.add(credit->second, latin_credit(id.artists));
                artists.push_back(std::move(id.credit));
                credit_tracks.emplace_back();
            }
            artist_ids.push_back(credit->second);
            credit_tracks[credit->second].push_back(slot);
            title_index.add(id.title.points);
            latin_titles.add(slot, latin_diacritics(id.title.points));
            bind_components(slot, id.title.points, false);
            bind_principal(id.principal, slot, false);
            representatives.push_back(track);
            identities.push_back({std::move(id.title), std::move(id.artists),
                                  std::move(id.principal), std::move(id.version),
                                  std::move(id.sections)});
        }
        for (const auto &[id, variants] : annotation_variants) {
            check_cancelled();
            for (const auto &parts : variants)
                if (parts.annotated && parts.base != identities[id].title.text &&
                    parts.base.find('/') != parts.base.npos) {
                    auto points = decode_utf8(parts.base);
                    bind_components(id, points, true);
                }
        }
        artist_index.freeze();
        title_index.freeze();
    }
    bool better(const Candidate &a, const Candidate &b) const noexcept {
        if (a.artist_gap != b.artist_gap)
            return !a.artist_gap;
        if (a.credit_gap != b.credit_gap)
            return !a.credit_gap;
        if (a.diacritic_gap != b.diacritic_gap)
            return !a.diacritic_gap;
        if (a.annotation_gap != b.annotation_gap)
            return !a.annotation_gap;
        if (a.version_gap != b.version_gap)
            return !a.version_gap;
        if (a.section_gap != b.section_gap)
            return !a.section_gap;
        if (a.compound_gap != b.compound_gap)
            return !a.compound_gap;
        const auto numerator = [](const Candidate &c) noexcept {
            return std::uint64_t(c.artist.numerator) * c.title.denominator +
                   std::uint64_t(c.title.numerator) * c.artist.denominator;
        };
        const auto denominator = [](const Candidate &c) noexcept {
            return std::uint64_t(c.artist.denominator) * c.title.denominator;
        };
        if (auto combined =
                compare_ratio(numerator(a), denominator(a), numerator(b), denominator(b));
            combined)
            return combined > 0;
        if (auto compared = compare(a.artist, b.artist); compared)
            return compared > 0;
        const auto &left = tracks[a.track_id];
        const auto &right = tracks[b.track_id];
        if (left.title != right.title)
            return left.title < right.title;
        return left.path.native() < right.path.native();
    }
    bool score_title(Candidate &candidate, std::u32string_view query, std::u32string_view local,
                     Fraction cutoff, const boost::container::static_vector<Candidate, 3> &best,
                     bool already_scored, bool exhaustive) const {
        if (!exhaustive && best.size() == best.capacity() && !better(candidate, best.back()))
            return false;
        if (already_scored)
            return true;
        auto length = std::max(query.size(), local.size());
        auto limit = static_cast<std::size_t>(std::uint64_t(cutoff.denominator - cutoff.numerator) *
                                              length / cutoff.denominator);
        if (!exhaustive && best.size() == best.capacity()) {
            auto can_improve = [&](std::size_t distance) noexcept {
                candidate.title = {static_cast<std::uint32_t>(length - distance),
                                   static_cast<std::uint32_t>(length)};
                return better(candidate, best.back());
            };
            if (!can_improve(limit)) {
                auto low = length - std::min(query.size(), local.size()), high = limit;
                while (low < high) {
                    auto middle = low + (high - low + 1) / 2;
                    if (can_improve(middle))
                        low = middle;
                    else
                        high = middle - 1;
                }
                limit = low;
            }
        }
        auto distance = edit_distance(query, local, limit);
        if (distance > limit)
            return false;
        candidate.title = {static_cast<std::uint32_t>(length - distance),
                           static_cast<std::uint32_t>(length)};
        return true;
    }
};
Index::Index(const std::vector<Track> &tracks) : data_(std::make_unique<Data>(tracks)) {}
Index::~Index() noexcept = default;
Index::Index(Index &&) noexcept = default;
Index &Index::operator=(Index &&) noexcept = default;
bool Index::owns(const Song &song) const {
    return data_->exact.contains(identity_key(song.title, song.artists));
}
bool Index::owns(const Identity &song) const {
    return data_->exact.contains(song.key);
}
std::size_t Index::unique_tracks() const noexcept {
    return data_->identities.size();
}
boost::container::static_vector<Candidate, 3> Index::suggest(const Song &song,
                                                             Fraction artist_cutoff,
                                                             Fraction title_cutoff,
                                                             bool exhaustive) const {
    return suggest(identity(song.title, song.artists), artist_cutoff, title_cutoff, exhaustive);
}
boost::container::static_vector<Candidate, 3> Index::suggest(const Identity &query,
                                                             Fraction artist_cutoff,
                                                             Fraction title_cutoff,
                                                             bool exhaustive) const {
    boost::unordered_flat_map<std::size_t, Fraction> artist_scores;
    for (auto id : data_->artist_index.query(query.credit.points, artist_cutoff, exhaustive)) {
        check_cancelled();
        auto score = similarity(query.credit.points, data_->artists[id].points, artist_cutoff);
        if (passes(score, artist_cutoff))
            artist_scores.emplace(id, score);
    }
    auto folded_credit = latin_credit(query.artists);
    boost::unordered_flat_set<std::size_t> diacritic_artists;
    for (auto credit : data_->latin_artists.query(
             query.credit.points, folded_credit, data_->artist_index,
             [&](std::size_t id) { return std::u32string_view(data_->artists[id].points); },
             [&](std::size_t id) {
                 return latin_credit(data_->identities[data_->credit_tracks[id].front()].artists);
             }, exhaustive)) {
        if (data_->identities[data_->credit_tracks[credit].front()].artists.size() !=
            query.artists.size())
            continue;
        diacritic_artists.insert(credit);
        if (!artist_scores.contains(credit))
            artist_scores.emplace(credit, complete_similarity(query.credit.points,
                                                              data_->artists[credit].points));
    }
    auto folded_title = latin_diacritics(query.title.points);
    boost::unordered_flat_set<std::size_t> diacritic_titles;
    for (auto id : data_->latin_titles.query(
             query.title.points, folded_title, data_->title_index,
             [&](std::size_t id) { return std::u32string_view(data_->identities[id].title.points); },
             [&](std::size_t id) { return latin_diacritics(data_->identities[id].title.points); },
             exhaustive))
        diacritic_titles.insert(id);
    boost::unordered_flat_set<std::size_t> credit_gaps;
    if (auto found = data_->principals.find(query.principal); found != data_->principals.end())
        for (auto id : data_->principal_tracks[found->second])
            if (data_->identities[id].artists != query.artists &&
                !diacritic_artists.contains(data_->artist_ids[id]))
                credit_gaps.insert(id);
    std::u32string extracted_query;
    if (query.annotations.annotated && !Data::base(query).empty() &&
        Data::base(query) != query.title.text)
        extracted_query = decode_utf8(Data::base(query));
    boost::container::small_vector<CompoundQuery, 2> query_forms;
    query_forms.emplace_back(query.title.points, false, !data_->annotation_titles.empty());
    if (!extracted_query.empty())
        query_forms.emplace_back(extracted_query, true, !data_->annotation_titles.empty());
    boost::unordered_flat_map<std::size_t, CompoundMatch> compound_matches;
    if (!exhaustive && (!artist_scores.empty() || !credit_gaps.empty()))
        compound_matches = data_->indexed_compounds(query_forms, [&](std::size_t id) {
            return artist_scores.contains(data_->artist_ids[id]) || credit_gaps.contains(id);
        });
    std::vector<std::size_t> candidates;
    if (!artist_scores.empty()) {
        std::size_t eligible{};
        for (const auto &[credit, score] : artist_scores)
            eligible += data_->credit_tracks[credit].size();
        if (!exhaustive && eligible <= data_->identities.size() / 4) {
            candidates.reserve(eligible);
            for (const auto &[credit, score] : artist_scores) {
                const auto &ids = data_->credit_tracks[credit];
                candidates.insert(candidates.end(), ids.begin(), ids.end());
            }
        } else {
            candidates = data_->title_index.query(query.title.points, title_cutoff, exhaustive);
            std::erase_if(candidates,
                          [&](auto id) { return !artist_scores.contains(data_->artist_ids[id]); });
            if (!exhaustive) {
                auto previous_size = candidates.size();
                if (auto found = data_->annotation_titles.find(Data::base(query));
                    found != data_->annotation_titles.end())
                    for (auto id : found->second)
                        if (artist_scores.contains(data_->artist_ids[id]))
                            candidates.push_back(id);
                if (query.annotations.annotated && !Data::base(query).empty()) {
                    const auto &points = extracted_query.empty() ? query.title.points
                                                                : extracted_query;
                    for (auto id : data_->title_index.query(points, {1, 1}, false))
                        if (artist_scores.contains(data_->artist_ids[id]) &&
                            data_->annotation_match(query, id))
                            candidates.push_back(id);
                }
                for (const auto &[id, match] : compound_matches)
                    if (artist_scores.contains(data_->artist_ids[id]))
                        candidates.push_back(id);
                for (auto id : diacritic_titles)
                    if (artist_scores.contains(data_->artist_ids[id]))
                        candidates.push_back(id);
                if (candidates.size() != previous_size) {
                    std::ranges::sort(candidates);
                    auto duplicate = std::ranges::unique(candidates);
                    candidates.erase(duplicate.begin(), duplicate.end());
                }
            }
        }
    }
    for (auto id : credit_gaps)
        if (!artist_scores.contains(data_->artist_ids[id]))
            candidates.push_back(id);
    boost::unordered_flat_set<std::size_t> overlapping_principals, artist_gaps;
    if (exhaustive) {
        for (std::size_t id = 0; id < data_->principal_names.size(); ++id)
            overlapping_principals.insert(id);
    } else {
        for (const auto &pair : artist_pairs(query.principal))
            if (auto found = data_->principal_pairs.find(pair);
                found != data_->principal_pairs.end())
                overlapping_principals.insert(found->second.begin(), found->second.end());
    }
    for (auto principal : overlapping_principals) {
        check_cancelled();
        if (!artist_overlap(query.principal, data_->principal_names[principal]))
            continue;
        for (auto id : data_->principal_tracks[principal])
            if ((query.title.text == data_->identities[id].title.text ||
                 data_->annotation_match(query, id)) &&
                !artist_scores.contains(data_->artist_ids[id]) && !credit_gaps.contains(id) &&
                artist_gaps.insert(id).second) {
                candidates.push_back(id);
            }
    }
    boost::unordered_flat_map<std::size_t, Fraction> review_scores;
    boost::container::static_vector<Candidate, 3> best;
    for (auto id : candidates) {
        check_cancelled();
        const auto &local = data_->identities[id];
        auto annotation_match = data_->annotation_match(query, id);
        auto version_gap = review_versions(local.version, query.version);
        auto compatible = local.version == query.version || version_gap;
        if (!compatible && !annotation_match)
            continue;
        CompoundMatch compound;
        if (exhaustive)
            compound = data_->exhaustive_compound(query_forms, id);
        else if (auto found = compound_matches.find(id); found != compound_matches.end())
            compound = found->second;
        auto annotation_gap =
            annotation_match && (!compatible || local.title.text != query.title.text);
        auto whole_annotation_gap = annotation_gap;
        annotation_gap |= compound.score.numerator && compound.annotated;
        version_gap |= annotation_gap && local.version != query.version;
        auto artist = artist_scores.find(data_->artist_ids[id]);
        auto gap = credit_gaps.contains(id);
        auto artist_gap = artist_gaps.contains(id);
        if (artist == artist_scores.end() && !gap && !artist_gap)
            continue;
        auto compound_gap = compound.score.numerator != 0;
        auto diacritic_title = diacritic_titles.contains(id);
        auto diacritic_gap = diacritic_title || diacritic_artists.contains(data_->artist_ids[id]);
        auto length = std::max(query.title.points.size(), local.title.points.size());
        auto shortest = std::min(query.title.points.size(), local.title.points.size());
        Fraction upper{static_cast<std::uint32_t>(shortest), static_cast<std::uint32_t>(length)};
        if (compound_gap)
            upper = compound.score;
        if (whole_annotation_gap)
            upper = {1, 1};
        if (diacritic_title && !compound_gap && !annotation_gap)
            upper = complete_similarity(query.title.points, local.title.points);
        if (!compound_gap && !annotation_gap && !diacritic_title && !passes(upper, title_cutoff))
            continue;
        auto artist_score = artist == artist_scores.end() ? Fraction{0, 1} : artist->second;
        if (artist == artist_scores.end() && (gap || artist_gap)) {
            auto credit = data_->artist_ids[id];
            auto [score, fresh] = review_scores.try_emplace(credit);
            if (fresh)
                score->second =
                    similarity(query.credit.points, data_->artists[credit].points, {0, 1});
            artist_score = score->second;
        }
        Candidate candidate{data_->representatives[id],
                            artist_score,
                            upper,
                            gap,
                            version_gap,
                            artist_gap,
                            local.sections != query.sections,
                            compound_gap,
                            annotation_gap,
                            diacritic_gap};
        if (!data_->score_title(candidate, query.title.points, local.title.points, title_cutoff,
                                best, compound_gap || annotation_gap || diacritic_title, exhaustive))
            continue;
        if (best.size() < best.capacity())
            best.push_back(candidate);
        else if (data_->better(candidate, best.back()))
            best.back() = candidate;
        else
            continue;
        std::ranges::sort(
            best, [&](const auto &a, const auto &b) noexcept { return data_->better(a, b); });
    }
    return best;
}
}
