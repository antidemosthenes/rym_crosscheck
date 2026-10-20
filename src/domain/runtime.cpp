#include "rym_crosscheck/domain/runtime.hpp"
#include <algorithm>
#include <cstddef>
#include <io.h>
#include <iostream>
#include <utility>
#include <vector>

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
    DWORD mode{};
    return _isatty(_fileno(stdin)) != 0 && GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode);
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
QuietProcess create_quiet_process(const std::filesystem::path &executable, std::wstring command) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle sink(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0,
                            nullptr));
    if (!sink)
        throw Error("Cannot isolate browser standard streams.");
    SIZE_T bytes{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (!bytes)
        throw Error("Cannot size browser process attributes.");
    std::vector<std::byte> storage(bytes);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
        throw Error("Cannot initialize browser process attributes.");
    struct Cleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~Cleanup() noexcept {
            DeleteProcThreadAttributeList(list);
        }
    } cleanup{attributes};
    auto inherited = sink.get();
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherited,
                                   sizeof(inherited), nullptr, nullptr))
        throw Error("Cannot restrict browser handle inheritance.");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError =
        sink.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, nullptr, &startup.StartupInfo, &info))
        throw Error("Cannot launch browser process (Windows error " +
                    std::to_string(GetLastError()) + ").");
    return {Handle(info.hProcess), Handle(info.hThread)};
}
FileLock::FileLock(const std::filesystem::path &path)
    : handle_(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr)) {
    if (!handle_) {
        auto failure = GetLastError();
        if (failure == ERROR_SHARING_VIOLATION || failure == ERROR_LOCK_VIOLATION)
            throw Error("Another run is using " + path_text(path) + ".");
        throw Error("Cannot open lock file " + path_text(path) + " (Windows error " +
                    std::to_string(failure) + ").");
    }
}
void Pacer::wait(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    auto check = [&] {
        check_cancelled();
        if (stop.stop_requested())
            throw RequestGroupStopped("Request group stopped.");
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
void Pacer::increase_interval(std::chrono::milliseconds minimum) {
    std::lock_guard lock(mutex_);
    interval_ = std::max(interval_, minimum);
    next_ = std::max(next_, std::chrono::steady_clock::now() + interval_);
}
}
