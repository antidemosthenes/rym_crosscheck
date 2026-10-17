#include "rym_crosscheck/cli/launcher.hpp"
#include <array>

namespace rym {
LauncherChannel::LauncherChannel(std::uint64_t output, std::uint64_t cancel, std::string id)
    : output_(reinterpret_cast<HANDLE>(output)), cancel_(reinterpret_cast<HANDLE>(cancel)),
      stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)), id_(std::move(id)) {
    DWORD flags{};
    auto cancellation = WaitForSingleObject(cancel_.get(), 0);
    if (!output_ || !cancel_ || !stop_ || id_.empty() || id_.size() > 100 ||
        !GetHandleInformation(output_.get(), &flags) || !(flags & HANDLE_FLAG_INHERIT) ||
        GetFileType(output_.get()) != FILE_TYPE_PIPE ||
        !GetHandleInformation(cancel_.get(), &flags) || !(flags & HANDLE_FLAG_INHERIT) ||
        (cancellation != WAIT_TIMEOUT && cancellation != WAIT_OBJECT_0))
        throw Error("Invalid private launcher channel.");
    watcher_ = std::jthread([this] {
        HANDLE handles[]{stop_.get(), cancel_.get()};
        if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
            cancelled.store(true);
    });
}
LauncherChannel::~LauncherChannel() noexcept {
    SetEvent(stop_.get());
    if (watcher_.joinable())
        watcher_.join();
}
void LauncherChannel::emit(std::string_view stage, boost::json::object fields) noexcept {
    try {
        std::lock_guard lock(output_mutex_);
        for (auto &field : fields)
            progress_[field.key()] = std::move(field.value());
        progress_["version"] = 1;
        progress_["id"] = id_;
        if (!stage.empty())
            progress_["stage"] = stage;
        auto line = boost::json::serialize(progress_);
        line += '\n';
        if (line.size() > 65536)
            return;
        DWORD written{};
        WriteFile(output_.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    } catch (...) {
    }
}
namespace {
struct Pipe {
    Handle read, write;
};
Pipe pipe() {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read{}, write{};
    if (!CreatePipe(&read, &write, &security, 65536))
        throw Error("Cannot create child process pipes.");
    Pipe result{Handle(read), Handle(write)};
    if (!SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0))
        throw Error("Cannot isolate child process pipes.");
    return result;
}
void drain(HANDLE pipe, std::string &text, bool tail) {
    DWORD available{};
    std::array<char, 8192> buffer;
    while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
        DWORD read{};
        if (!ReadFile(pipe, buffer.data(),
                      std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read,
                      nullptr) ||
            !read)
            break;
        text.append(buffer.data(), read);
        if (tail && text.size() > 256 * 1024)
            text.erase(0, text.size() - 256 * 1024);
        if (!tail && text.size() > 128 * 1024)
            throw Error("Launcher progress exceeded its size limit.");
    }
}
}
RunSupervisor::RunSupervisor(const std::filesystem::path &executable,
                             std::vector<std::string> arguments, Progress progress,
                             Finished finished)
    : cancel_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!cancel_)
        throw Error("Cannot create comparison cancellation event.");
    worker_ = std::jthread([this, executable, arguments = std::move(arguments),
                            progress = std::move(progress),
                            finished = std::move(finished)]() mutable {
        RunOutcome outcome;
        try {
            auto events = pipe(), logs = pipe();
            HANDLE inherited_cancel{};
            if (!DuplicateHandle(GetCurrentProcess(), cancel_.get(), GetCurrentProcess(),
                                 &inherited_cancel, SYNCHRONIZE, TRUE, 0))
                throw Error("Cannot share comparison cancellation event.");
            Handle child_cancel(inherited_cancel);
            SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
            Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &security, OPEN_EXISTING, 0, nullptr));
            Handle job(CreateJobObjectW(nullptr, nullptr));
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!input || !job ||
                !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                         sizeof(limits)))
                throw Error("Cannot own comparison process cleanup.");
            auto id =
                std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
            arguments.insert(arguments.end(),
                             {"--launcher-events",
                              std::to_string(reinterpret_cast<std::uintptr_t>(events.write.get())),
                              "--launcher-cancel",
                              std::to_string(reinterpret_cast<std::uintptr_t>(child_cancel.get())),
                              "--launcher-id", id});
            auto command = windows_command(executable, arguments);
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = input.get();
            startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = logs.write.get();
            SIZE_T bytes{};
            InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            std::vector<std::byte> attributes(bytes);
            startup.lpAttributeList =
                reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
            if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes))
                throw Error("Cannot initialize child handle isolation.");
            struct Attributes {
                LPPROC_THREAD_ATTRIBUTE_LIST value;
                ~Attributes() noexcept {
                    DeleteProcThreadAttributeList(value);
                }
            } cleanup{startup.lpAttributeList};
            HANDLE handles[]{input.get(), logs.write.get(), events.write.get(), child_cancel.get()};
            if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                           PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                           sizeof(handles), nullptr, nullptr))
                throw Error("Cannot restrict inherited comparison handles.");
            PROCESS_INFORMATION info{};
            if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                                nullptr, executable.parent_path().c_str(), &startup.StartupInfo,
                                &info))
                throw Error("Cannot start bin/rym-crosscheck.exe (Windows error " +
                            std::to_string(GetLastError()) + ").");
            Handle process(info.hProcess), thread(info.hThread);
            if (!AssignProcessToJobObject(job.get(), process.get())) {
                TerminateProcess(process.get(), 1);
                throw Error("Cannot supervise comparison cleanup.");
            }
            if (ResumeThread(thread.get()) == DWORD(-1))
                throw Error("Cannot resume comparison process.");
            events.write = Handle{};
            logs.write = Handle{};
            child_cancel = Handle{};
            input = Handle{};
            std::string pending;
            std::optional<boost::json::object> latest;
            auto delivered = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            auto receive = [&] {
                drain(events.read.get(), pending, false);
                std::size_t end{};
                while ((end = pending.find('\n')) != pending.npos) {
                    auto event =
                        boost::json::parse(std::string_view(pending).substr(0, end)).as_object();
                    if (event.at("version").to_number<int>() != 1 ||
                        event.at("id").as_string() != id)
                        throw Error("Invalid comparison progress protocol.");
                    latest = std::move(event);
                    pending.erase(0, end + 1);
                }
                drain(logs.read.get(), outcome.diagnostics, true);
                auto now = std::chrono::steady_clock::now();
                if (latest && now - delivered >= std::chrono::milliseconds(250)) {
                    progress(std::move(*latest));
                    latest.reset();
                    delivered = now;
                }
            };
            while (WaitForSingleObject(process.get(), 50) == WAIT_TIMEOUT)
                receive();
            receive();
            DWORD code{};
            if (!GetExitCodeProcess(process.get(), &code))
                throw Error("Cannot read comparison exit status.");
            outcome.exit_code = static_cast<int>(code);
        } catch (const std::exception &error) {
            outcome.diagnostics += "\n" + std::string(error.what());
        }
        outcome.cancellation_requested = WaitForSingleObject(cancel_.get(), 0) == WAIT_OBJECT_0;
        if (!outcome.diagnostics.empty()) {
            auto length =
                MultiByteToWideChar(CP_UTF8, 0, outcome.diagnostics.data(),
                                    static_cast<int>(outcome.diagnostics.size()), nullptr, 0);
            if (length > 0) {
                std::wstring decoded(static_cast<std::size_t>(length), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, outcome.diagnostics.data(),
                                    static_cast<int>(outcome.diagnostics.size()), decoded.data(),
                                    length);
                outcome.diagnostics = utf8(decoded);
                if (outcome.diagnostics.size() > 256 * 1024) {
                    auto start = outcome.diagnostics.size() - 256 * 1024;
                    while (start < outcome.diagnostics.size() &&
                           (static_cast<unsigned char>(outcome.diagnostics[start]) & 0xc0) == 0x80)
                        ++start;
                    outcome.diagnostics.erase(0, start);
                }
            } else
                outcome.diagnostics = "The CLI returned diagnostic text that could not be decoded.";
        }
        try {
            finished(std::move(outcome));
        } catch (...) {
        }
    });
}
RunSupervisor::~RunSupervisor() noexcept {
    cancel();
    if (worker_.joinable())
        worker_.join();
}
void RunSupervisor::cancel() noexcept {
    SetEvent(cancel_.get());
}
}
