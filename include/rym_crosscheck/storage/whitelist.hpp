#pragma once
#include "rym_crosscheck/domain/models.hpp"
#include <boost/unordered/unordered_flat_set.hpp>
namespace rym {
struct Identity;
struct WhitelistEntry {
    std::string key, url, title;
    std::vector<std::string> artists;
};
class Whitelist {
    boost::unordered_flat_set<std::string> urls_, identities_;
    std::vector<WhitelistEntry> entries_;

  public:
    Whitelist() = default;
    Whitelist(const std::filesystem::path &path, bool required);
    explicit Whitelist(std::vector<WhitelistEntry> entries);
    static Whitelist parse(std::string_view text);
    const std::vector<WhitelistEntry> &entries() const noexcept {
        return entries_;
    }
    std::string toml() const;
    bool contains(const Song &song) const;
    bool contains(const Song &song, const Identity &normalized) const;
};
}
