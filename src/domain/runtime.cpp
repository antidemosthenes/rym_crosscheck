#include "rym_crosscheck/domain/runtime.hpp"
#include <io.h>
#include <iostream>
#include <utility>

namespace rym {
std::atomic_bool cancelled{};
namespace {
BOOL WINAPI control(DWORD event) noexcept {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        cancelled.store(true);
        return TRUE;
    }
    return FALSE;
}
}
void check_cancelled() {
    if (cancelled.load())
        throw Error("Cancelled.");
}
void install_cancellation() noexcept {
    cancelled.store(false);
    SetConsoleCtrlHandler(control, TRUE);
}
bool interactive() noexcept {
    return _isatty(_fileno(stdin)) != 0;
}
std::wstring wide(std::string_view value) {
    if (value.empty())
        return {};
    auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                    static_cast<int>(value.size()), nullptr, 0);
    if (!size)
        throw Error("Invalid UTF-8.");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}
std::string utf8(std::wstring_view value) {
    if (value.empty())
        return {};
    auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                    static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size)
        throw Error("Invalid UTF-16.");
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}
std::string path_text(const std::filesystem::path &path) {
    return utf8(path.native());
}
std::wstring environment(std::wstring_view name) {
    std::wstring key(name);
    auto size = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (!size)
        return {};
    std::wstring value(size, L'\0');
    auto written = GetEnvironmentVariableW(key.c_str(), value.data(), size);
    if (!written || written >= size)
        throw Error("Environment variable changed while reading.");
    value.resize(written);
    return value;
}
std::filesystem::path default_profile() {
    const auto base = environment(L"LOCALAPPDATA");
    if (base.empty())
        throw Error("LOCALAPPDATA is unavailable; use --browser-profile.");
    return std::filesystem::path(base) / L"rym-crosscheck" / L"edge-profile";
}
std::int64_t now_seconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
void log(std::string_view message) {
    static std::mutex mutex;
    std::lock_guard guard(mutex);
    std::cerr << message << '\n';
}
Handle::~Handle() noexcept {
    if (*this)
        CloseHandle(value_);
}
Handle::Handle(Handle &&other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
Handle &Handle::operator=(Handle &&other) noexcept {
    if (this != &other) {
        if (*this)
            CloseHandle(value_);
        value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
}
FileLock::FileLock(const std::filesystem::path &path)
    : handle_(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr)) {
    if (!handle_)
        throw Error("Another run is using " + path_text(path) + ".");
}
void Pacer::wait(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    auto check = [&] {
        check_cancelled();
        if (stop.stop_requested())
            throw Error("Request group stopped.");
    };
    auto current = std::chrono::steady_clock::now();
    while (current < next_) {
        check();
        std::this_thread::sleep_for(
            std::min(std::chrono::milliseconds(50),
                     std::chrono::duration_cast<std::chrono::milliseconds>(next_ - current)));
        current = std::chrono::steady_clock::now();
    }
    check();
    next_ = current + interval_;
}
}
