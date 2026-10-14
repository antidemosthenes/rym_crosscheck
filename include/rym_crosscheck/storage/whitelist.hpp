#pragma once
#include "rym_crosscheck/domain/models.hpp"
#include <boost/unordered/unordered_flat_set.hpp>
namespace rym {
struct Identity;
class Whitelist {
    boost::unordered_flat_set<std::string> urls_, identities_;

  public:
    Whitelist(const std::filesystem::path &path, bool required);
    bool contains(const Song &song) const;
    bool contains(const Song &song, const Identity &normalized) const;
};
}
