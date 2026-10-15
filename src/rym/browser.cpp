#include "rym_crosscheck/rym/parser.hpp"
#include "rym_crosscheck/rym/transport.hpp"
#include "rym_crosscheck/storage/files.hpp"
#include <algorithm>
#include <boost/json.hpp>
#include <charconv>
#include <conio.h>
#include <fstream>
#include <winhttp.h>

namespace rym {
namespace {
struct InternetClose {
    void operator()(void *handle) const noexcept {
        if (handle)
            WinHttpCloseHandle(handle);
    }
};
using Internet = std::unique_ptr<void, InternetClose>;
std::wstring quote(std::wstring_view argument) {
    std::wstring result = L"\"";
    std::size_t slashes{};
    for (auto c : argument) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"')
            result.append(slashes * 2 + 1, L'\\');
        else
            result.append(slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}
std::filesystem::path edge_path() {
    for (const auto *variable : {L"ProgramFiles(x86)", L"ProgramFiles", L"LOCALAPPDATA"})
        if (const auto base = environment(variable); !base.empty()) {
            auto path = std::filesystem::path(base) / L"Microsoft/Edge/Application/msedge.exe";
            if (std::filesystem::exists(path))
                return path;
        }
    throw Error("Microsoft Edge is not installed in a standard location.");
}
class Socket {
    Internet session_, connection_, socket_;

  public:
    explicit Socket(std::wstring_view endpoint) {
        std::wstring url(endpoint);
        if (url.starts_with(L"ws://"))
            url.replace(0, 5, L"http://");
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = parts.dwUrlPathLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
            throw Error("Invalid local browser endpoint.");
        auto host = std::wstring(parts.lpszHostName, parts.dwHostNameLength);
        if (host != L"127.0.0.1" && host != L"localhost")
            throw Error("Browser endpoint must be local.");
        session_.reset(
            WinHttpOpen(L"rym-crosscheck/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0));
        if (!session_)
            throw Error("Cannot initialize browser connection.");
        WinHttpSetTimeouts(session_.get(), 2000, 2000, 2000, 2000);
        connection_.reset(WinHttpConnect(session_.get(), host.c_str(), parts.nPort, 0));
        Internet request(
            WinHttpOpenRequest(connection_.get(), L"GET",
                               std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength).c_str(),
                               nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
        if (!request ||
            !WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
            !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.get(), nullptr))
            throw Error("Cannot connect to Edge debugging socket.");
        socket_.reset(WinHttpWebSocketCompleteUpgrade(request.get(), 0));
        if (!socket_)
            throw Error("Edge WebSocket upgrade failed.");
        DWORD timeout = 2000;
        WinHttpSetOption(socket_.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    }
    void send(std::string &message) {
        auto error =
            WinHttpWebSocketSend(socket_.get(), WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 message.data(), static_cast<DWORD>(message.size()));
        if (error)
            throw Error("Browser disconnected while sending a command.");
    }
    std::string receive(bool cleanup) {
        std::string message;
        char buffer[32768];
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cleanup ? 2 : 15);
        for (;;) {
            if (!cleanup)
                check_cancelled();
            DWORD bytes{};
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            auto error =
                WinHttpWebSocketReceive(socket_.get(), buffer, sizeof(buffer), &bytes, &type);
            if (error == ERROR_WINHTTP_TIMEOUT && std::chrono::steady_clock::now() < deadline)
                continue;
            if (error || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                throw Error("Browser disconnected or stopped responding.");
            if (message.size() + bytes > 32 * 1024 * 1024)
                throw Error("Browser message exceeds 32 MiB.");
            message.append(buffer, bytes);
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                return message;
            if (std::chrono::steady_clock::now() >= deadline)
                throw Error("Browser response timed out.");
        }
    }
};
class Session {
  public:
    std::filesystem::path profile;
    FileLock lock;
    Handle job, process;
    std::unique_ptr<Socket> socket;
    Pacer &pacer;
    std::mutex commands;
    std::stop_source restricted;
    std::string user_agent;
    std::int64_t sequence{};
    Session(std::filesystem::path path, Pacer &clock)
        : profile(std::move(path)), lock(profile / L"rym-run.lock"), pacer(clock) {
        auto executable = edge_path();
        auto port_file = profile / L"DevToolsActivePort";
        std::error_code error;
        std::filesystem::remove(port_file, error);
        job = Handle(CreateJobObjectW(nullptr, nullptr));
        if (!job)
            throw Error("Cannot create browser process owner.");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits)))
            throw Error("Cannot configure browser process cleanup.");
        auto command =
            quote(executable.native()) + L" --user-data-dir=" + quote(profile.native()) +
            L" --remote-debugging-address=127.0.0.1 --remote-debugging-port=0 --disable-extensions "
            L"--no-first-run --no-default-browser-check --headless=new about:blank";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup,
                            &info))
            throw Error("Cannot launch Edge.");
        process = Handle(info.hProcess);
        Handle thread(info.hThread);
        if (!AssignProcessToJobObject(job.get(), process.get())) {
            TerminateProcess(process.get(), 1);
            throw Error("Cannot own Edge process cleanup.");
        }
        ResumeThread(thread.get());
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::string port_text, socket_path;
        while (std::chrono::steady_clock::now() < deadline) {
            check_cancelled();
            if (!alive())
                throw Error("Edge exited; the dedicated profile may already be in use or debugging "
                            "may be disabled.");
            std::ifstream input(port_file);
            if (input && std::getline(input, port_text) && std::getline(input, socket_path) &&
                !port_text.empty() && socket_path.starts_with("/devtools/browser/"))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        unsigned port{};
        auto [end, code] =
            std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (code != std::errc{} || end != port_text.data() + port_text.size() || !port ||
            port > 65535 || socket_path.empty())
            throw Error("Edge did not publish a local debugging endpoint.");
        socket = std::make_unique<Socket>(wide("ws://127.0.0.1:" + port_text + socket_path));
        user_agent = this->command("Browser.getVersion").at("userAgent").as_string().c_str();
        // Keep retrieval identity consistent with cookies issued during visible verification.
        if (auto headless = user_agent.find("HeadlessChrome"); headless != user_agent.npos)
            user_agent.replace(headless, std::string_view("HeadlessChrome").size(), "Chrome");
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            auto targets = this->command("Target.getTargets").at("targetInfos").as_array();
            if (std::ranges::any_of(targets, [](const auto &target) {
                    return target.as_object().at("type").as_string() == "page";
                }))
                break;
            if (std::chrono::steady_clock::now() >= deadline)
                throw Error("Edge did not initialize its background page.");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    ~Session() noexcept {
        if (socket) {
            try {
                command("Browser.close", {}, {}, true);
            } catch (...) {
            }
            socket.reset();
        }
        for (int i = 0; i < 30 && alive(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (alive())
            TerminateJobObject(job.get(), 0);
        try {
            std::error_code error;
            std::filesystem::remove(profile / L"DevToolsActivePort", error);
        } catch (...) {
        }
    }
    bool alive() const noexcept {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        return QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &info,
                                         sizeof(info), nullptr) &&
               info.ActiveProcesses != 0;
    }
    boost::json::object command(std::string_view method, boost::json::object parameters = {},
                                std::string_view session_id = {}, bool cleanup = false) {
        std::lock_guard guard(commands);
        if (!cleanup)
            check_cancelled();
        if (!alive())
            throw Error("Edge was closed.");
        auto id = ++sequence;
        boost::json::object request{
            {"id", id}, {"method", method}, {"params", std::move(parameters)}};
        if (!session_id.empty())
            request["sessionId"] = session_id;
        auto encoded = boost::json::serialize(request);
        socket->send(encoded);
        for (;;) {
            boost::json::monotonic_resource resource;
            auto value = boost::json::parse(socket->receive(cleanup), &resource);
            const auto &object = value.as_object();
            auto *response_id = object.if_contains("id");
            if (!response_id || response_id->to_number<std::int64_t>() != id)
                continue;
            if (auto *failure = object.if_contains("error"))
                throw Error("Edge command failed: " + boost::json::serialize(*failure));
            return boost::json::object(object.at("result").as_object(), boost::json::storage_ptr{});
        }
    }
};
std::filesystem::path prepare_profile(const std::filesystem::path &path) {
    auto result = std::filesystem::absolute(path).lexically_normal();
    std::filesystem::create_directories(result);
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(result)))
        throw Error("Browser profile cannot be a symbolic link.");
    if (result.filename() == "User Data")
        throw Error("Use a dedicated application profile, not personal browser User Data.");
    return result;
}
}
struct Browser::Data {
    std::shared_ptr<Session> browser;
    std::string target, session;
    explicit Data(std::shared_ptr<Session> shared) : browser(std::move(shared)) {
        boost::json::object parameters{
            {"url", "about:blank"}, {"background", true}, {"hidden", true}};
        auto created = browser->command("Target.createTarget", std::move(parameters));
        target = created.at("targetId").as_string().c_str();
        try {
            session =
                browser->command("Target.attachToTarget", {{"targetId", target}, {"flatten", true}})
                    .at("sessionId")
                    .as_string()
                    .c_str();
            browser->command("Page.enable", {}, session);
            browser->command("Network.enable", {}, session);
            browser->command("Network.setUserAgentOverride", {{"userAgent", browser->user_agent}},
                             session);
        } catch (...) {
            close();
            throw;
        }
    }
    ~Data() noexcept {
        if (browser.use_count() == 1) {
            try {
                browser->command("Network.clearBrowserCache", {}, session, true);
            } catch (...) {
            }
        }
        close();
    }
    void close() noexcept {
        try {
            browser->command("Target.closeTarget", {{"targetId", target}}, {}, true);
        } catch (...) {
        }
    }
};
void verify_browser(const std::filesystem::path &path, std::chrono::seconds timeout) {
    if (!interactive())
        throw Error("Verification requires an interactive terminal.");
    auto profile = prepare_profile(path);
    FileLock lock(profile / L"rym-run.lock");
    Handle job(CreateJobObjectW(nullptr, nullptr)), process;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                         sizeof(limits)))
        throw Error("Cannot own verification browser.");
    struct Cleanup {
        HANDLE job;
        ~Cleanup() noexcept {
            EnumWindows(
                [](HWND window, LPARAM value) noexcept -> BOOL {
                    DWORD pid{};
                    GetWindowThreadProcessId(window, &pid);
                    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
                    BOOL owned{};
                    if (process &&
                        IsProcessInJob(process.get(), reinterpret_cast<HANDLE>(value), &owned) &&
                        owned)
                        PostMessageW(window, WM_CLOSE, 0, 0);
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(job));
            for (int i = 0; i < 30; ++i) {
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
                if (QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info,
                                              sizeof(info), nullptr) &&
                    !info.ActiveProcesses)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            TerminateJobObject(job, 0);
        }
    } cleanup{job.get()};
    auto executable = edge_path();
    auto command = quote(executable.native()) + L" --user-data-dir=" + quote(profile.native()) +
                   L" --disable-extensions --no-first-run --no-default-browser-check "
                   L"https://rateyourmusic.com/charts/top/song/all-time/";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup,
                        &info))
        throw Error("Cannot open normal verification browser.");
    process = Handle(info.hProcess);
    Handle thread(info.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 1);
        throw Error("Cannot own verification browser cleanup.");
    }
    ResumeThread(thread.get());
    log("Complete verification in normal Edge (extensions disabled), then press Enter in this "
        "terminal or close that browser window. The program will reuse its cookies.");
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        check_cancelled();
        if (_kbhit()) {
            auto key = _getwch();
            if (key == L'\r' || key == L'\n')
                return;
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION active{};
        if (QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &active,
                                      sizeof(active), nullptr) &&
            !active.ActiveProcesses)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw Error("Normal-browser verification timed out; previous report preserved.");
}
Browser::Browser(const std::filesystem::path &profile, Pacer &pacer)
    : data_(std::make_unique<Data>(std::make_shared<Session>(prepare_profile(profile), pacer))) {}
Browser::Browser(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
Browser::~Browser() noexcept = default;
Browser::Browser(Browser &&) noexcept = default;
Browser &Browser::operator=(Browser &&) noexcept = default;
std::unique_ptr<Transport> Browser::fork() {
    return std::unique_ptr<Transport>(new Browser(std::make_unique<Data>(data_->browser)));
}
std::string Browser::fetch(std::string_view url) {
    auto target = site_url(url);
    auto &state = *data_->browser;
    state.pacer.wait(state.restricted.get_token());
    state.command("Page.navigate", {{"url", target}}, data_->session);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        check_cancelled();
        boost::json::object evaluated;
        try {
            evaluated = state.command(
                "Runtime.evaluate",
                {{"expression", "({url:location.href,ready:document.readyState,html:document."
                                "documentElement?document.documentElement.outerHTML:''})"},
                 {"returnByValue", true}},
                data_->session);
        } catch (const Error &error) {
            check_cancelled();
            if (!state.alive() || std::string_view(error.what()).find("Edge command failed:") ==
                                      std::string_view::npos)
                throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        auto *result = evaluated.if_contains("result");
        if (!result || !result->is_object() || !result->as_object().if_contains("value")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const auto &value = result->as_object().at("value").as_object();
        auto current = std::string(value.at("url").as_string());
        auto html = std::string(value.at("html").as_string());
        auto ready = value.at("ready").as_string();
        if (current == "about:blank" || html.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (!current.starts_with("https://rateyourmusic.com/"))
            throw Error("Browser navigated outside RYM.");
        auto canonical = [](std::string value) {
            auto fragment = value.find('#');
            if (fragment != value.npos)
                value.resize(fragment);
            while (value.ends_with('/'))
                value.pop_back();
            return value;
        };
        if (!challenged(html) && canonical(current) != canonical(target)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (challenged(html)) {
            state.restricted.request_stop();
            if (!interactive())
                throw Error("RYM verification requires an interactive terminal; rerun "
                            "interactively or use --offline.");
            throw VerificationRequired("RYM needs verification in an ordinary browser session.");
        } else if (ready == "complete" &&
                   (target.find("/charts/") == std::string::npos ||
                    html.find("page_charts_section_charts_item") != std::string::npos)) {
            if (html.size() > 16 * 1024 * 1024)
                throw Error("HTML exceeds 16 MiB.");
            return html;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    throw Error("RYM page did not become usable; access may be blocked or markup changed.");
}
}
