#include "rym_crosscheck/matching/index.hpp"
#include "rym_crosscheck/domain/runtime.hpp"
#include <algorithm>
#include <array>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <limits>
#include <numeric>

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
int compare(Fraction a, Fraction b) noexcept {
    auto left = std::uint64_t(a.numerator) * b.denominator;
    auto right = std::uint64_t(b.numerator) * a.denominator;
    return (left > right) - (left < right);
}
int compare_ratio(std::uint64_t an, std::uint64_t ad, std::uint64_t bn, std::uint64_t bd) noexcept {
    int direction = 1;
    for (;;) {
        auto aq = an / ad, bq = bn / bd;
        if (aq != bq)
            return direction * ((aq > bq) - (aq < bq));
        auto ar = an % ad, br = bn % bd;
        if (!ar || !br)
            return direction * ((ar > br) - (ar < br));
        an = ad;
        ad = ar;
        bn = bd;
        bd = br;
        direction = -direction;
    }
}
}
struct Index::Data {
    const std::vector<Track> &tracks;
    std::vector<Identity> identities;
    std::vector<std::size_t> representatives;
    std::vector<Name> artists;
    std::vector<std::size_t> artist_ids;
    boost::unordered_flat_map<std::string, std::size_t> exact;
    boost::unordered_flat_map<std::string, std::vector<std::size_t>> principals;
    NameIndex artist_index, title_index;
    explicit Data(const std::vector<Track> &source) : tracks(source) {
        identities.reserve(source.size());
        representatives.reserve(source.size());
        artist_ids.reserve(source.size());
        exact.reserve(source.size());
        boost::unordered_flat_map<std::string, std::size_t> credit_ids;
        for (std::size_t track = 0; track < source.size(); ++track) {
            auto id = identity(source[track].title, source[track].artists);
            auto [existing, inserted] = exact.emplace(id.key, identities.size());
            if (!inserted) {
                auto &representative = representatives[existing->second];
                if (source[track].path.native() < source[representative].path.native())
                    representative = track;
                continue;
            }
            auto slot = identities.size();
            auto [credit, fresh] = credit_ids.emplace(id.credit.text, artists.size());
            if (fresh) {
                artist_index.add(id.credit.points);
                artists.push_back(id.credit);
            }
            artist_ids.push_back(credit->second);
            title_index.add(id.title.points);
            principals[id.principal].push_back(slot);
            representatives.push_back(track);
            identities.push_back(std::move(id));
        }
        artist_index.freeze();
        title_index.freeze();
    }
    bool better(const Candidate &a, const Candidate &b) const noexcept {
        if (a.credit_gap != b.credit_gap)
            return !a.credit_gap;
        if (a.version_gap != b.version_gap)
            return !a.version_gap;
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
};
Index::Index(const std::vector<Track> &tracks) : data_(std::make_unique<Data>(tracks)) {}
Index::~Index() noexcept = default;
Index::Index(Index &&) noexcept = default;
Index &Index::operator=(Index &&) noexcept = default;
bool Index::owns(const Song &song) const {
    return owns(identity(song.title, song.artists));
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
    auto candidates = artist_scores.empty()
                          ? std::vector<std::size_t>{}
                          : data_->title_index.query(query.title.points, title_cutoff, exhaustive);
    boost::unordered_flat_set<std::size_t> credit_gaps;
    if (auto found = data_->principals.find(query.principal); found != data_->principals.end())
        for (auto id : found->second)
            if (data_->identities[id].artists != query.artists) {
                credit_gaps.insert(id);
                candidates.push_back(id);
            }
    boost::container::static_vector<Candidate, 3> best;
    for (auto id : candidates) {
        check_cancelled();
        const auto &local = data_->identities[id];
        auto version_gap = review_versions(local.version, query.version);
        if (local.version != query.version && !version_gap)
            continue;
        auto artist = artist_scores.find(data_->artist_ids[id]);
        auto gap = credit_gaps.contains(id);
        if (artist == artist_scores.end() && !gap)
            continue;
        auto title_score = similarity(query.title.points, local.title.points, title_cutoff);
        if (!passes(title_score, title_cutoff))
            continue;
        Candidate candidate{data_->representatives[id],
                            artist == artist_scores.end() ? Fraction{0, 1} : artist->second,
                            title_score, gap, version_gap};
        if (std::ranges::any_of(best, [&](const Candidate &existing) {
                return existing.track_id == candidate.track_id;
            }))
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
