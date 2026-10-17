#pragma once
#include "rym_crosscheck/cli/options.hpp"
#include <functional>

namespace rym {
class LauncherChannel {
    Handle output_, cancel_, stop_;
    std::string id_;
    std::mutex output_mutex_;
    boost::json::object progress_;
    std::jthread watcher_;

  public:
    LauncherChannel(std::uint64_t output, std::uint64_t cancel, std::string id);
    ~LauncherChannel() noexcept;
    void emit(std::string_view stage, boost::json::object fields = {}) noexcept;
};
struct RunOutcome {
    int exit_code{1};
    bool cancellation_requested{};
    std::string diagnostics;
};
class RunSupervisor {
    Handle cancel_;
    std::jthread worker_;

  public:
    using Progress = std::function<void(boost::json::object)>;
    using Finished = std::function<void(RunOutcome)>;
    RunSupervisor(const std::filesystem::path &executable, std::vector<std::string> arguments,
                  Progress progress, Finished finished);
    ~RunSupervisor() noexcept;
    void cancel() noexcept;
    RunSupervisor(const RunSupervisor &) = delete;
    RunSupervisor &operator=(const RunSupervisor &) = delete;
};
}
