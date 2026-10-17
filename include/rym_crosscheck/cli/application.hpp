#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace rym {
int application(const std::vector<std::string> &arguments,
                const std::filesystem::path &application_directory = {});
}
