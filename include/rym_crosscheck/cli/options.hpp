#pragma once
#include "rym_crosscheck/report/report.hpp"
#include <algorithm>

namespace rym {
struct ComparisonOptions {
    std::string music, cache, profile, artist{"0.90"}, title{"0.85"};
    std::size_t top{200},
        local_workers{std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8)},
        web_workers{4};
    int delay{1000}, verification{300};
    bool offline{}, clear{}, no_open{};
};
ComparisonOptions default_options(const PortablePaths &paths);
ComparisonOptions validate_options(ComparisonOptions value);
boost::json::object options_json(const ComparisonOptions &value);
ComparisonOptions parse_options(const boost::json::object &value, const PortablePaths &paths);
ComparisonOptions load_settings(const PortablePaths &paths);
void save_settings(const PortablePaths &paths, const ComparisonOptions &value);
std::vector<std::string> comparison_arguments(const ComparisonOptions &value);
std::wstring windows_command(const std::filesystem::path &executable,
                             const std::vector<std::string> &arguments);
}
