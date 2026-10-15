#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rym {
class Error : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
extern std::atomic_bool cancelled;
void check_cancelled();
void install_cancellation() noexcept;
bool interactive() noexcept;
std::wstring wide(std::string_view value);
std::string utf8(std::wstring_view value);
std::string path_text(const std::filesystem::path &path);
std::wstring environment(std::wstring_view name);
std::filesystem::path default_profile();
std::int64_t now_seconds() noexcept;
void log(std::string_view message);
class Handle {
    HANDLE value_{};

  public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() noexcept;
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept;
    Handle &operator=(Handle &&other) noexcept;
    HANDLE get() const noexcept {
        return value_;
    }
    explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
};
class FileLock {
    Handle handle_;

  public:
    explicit FileLock(const std::filesystem::path &path);
};
class Pacer {
    std::mutex mutex_;
    std::chrono::steady_clock::time_point next_{};
    std::chrono::milliseconds interval_;

  public:
    explicit Pacer(std::chrono::milliseconds interval) noexcept : interval_(interval) {}
    void wait(std::stop_token stop = {});
};
}
