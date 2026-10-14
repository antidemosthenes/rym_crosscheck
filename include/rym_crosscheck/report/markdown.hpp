#pragma once
#include "rym_crosscheck/domain/models.hpp"
namespace rym {
std::string markdown(const Snapshot &snapshot, const Scan &scan,
                     const std::vector<Missing> &missing, const std::filesystem::path &root,
                     bool offline);
}
