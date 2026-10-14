#pragma once
#include "rym_crosscheck/domain/models.hpp"
#include "rym_crosscheck/storage/files.hpp"
#include <boost/unordered/unordered_flat_map.hpp>
namespace rym {
class Cache {
    std::filesystem::path root_;
    FileLock lock_;

  public:
    explicit Cache(std::filesystem::path root, bool clear = false);
    boost::unordered_flat_map<std::string, Track>
    load_tracks(const std::filesystem::path &root) const;
    void save_tracks(const std::filesystem::path &root, const std::vector<Track> &tracks) const;
    void save_snapshot(const Snapshot &snapshot) const;
    Snapshot load_snapshot(std::size_t top) const;
    void prune() const;
};
}
