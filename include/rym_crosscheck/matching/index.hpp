#pragma once
#include "rym_crosscheck/matching/normalize.hpp"
#include <memory>
namespace rym {
class Index {
    struct Data;
    std::unique_ptr<Data> data_;

  public:
    explicit Index(const std::vector<Track> &tracks);
    ~Index() noexcept;
    Index(const Index &) = delete;
    Index &operator=(const Index &) = delete;
    Index(Index &&) noexcept;
    Index &operator=(Index &&) noexcept;
    bool owns(const Song &song) const;
    bool owns(const Identity &song) const;
    boost::container::static_vector<Candidate, 3>
    suggest(const Song &song, Fraction artist, Fraction title, bool exhaustive = false) const;
    boost::container::static_vector<Candidate, 3>
    suggest(const Identity &song, Fraction artist, Fraction title, bool exhaustive = false) const;
    std::size_t unique_tracks() const noexcept;
};
}
