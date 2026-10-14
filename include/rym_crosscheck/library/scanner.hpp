#pragma once
#include "rym_crosscheck/storage/cache.hpp"
namespace rym {
Track read_track(const std::filesystem::path &path);
Scan scan_library(const std::filesystem::path &root, const Cache &cache, std::size_t workers);
}
