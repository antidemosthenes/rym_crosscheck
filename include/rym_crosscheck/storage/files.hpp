#pragma once
#include "rym_crosscheck/domain/runtime.hpp"
#include <boost/json.hpp>
namespace rym {
std::string read_file(const std::filesystem::path &path, std::size_t maximum = 128 * 1024 * 1024);
void atomic_write(const std::filesystem::path &path, std::string_view value);
boost::json::value read_json(const std::filesystem::path &path);
void write_json(const std::filesystem::path &path, const boost::json::value &value);
std::int64_t modified_time(const std::filesystem::path &path);
}
