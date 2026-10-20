#include "rym_crosscheck/rym/parser.hpp"
#include "rym_crosscheck/rym/socket.hpp"
#include "rym_crosscheck/rym/transport.hpp"
#include "rym_crosscheck/storage/files.hpp"
#include <algorithm>
#include <boost/json.hpp>
#include <charconv>
#include <conio.h>
#include <fstream>
#include <utility>

namespace rym {
namespace {
bool same_url_text(std::string_view left, std::string_view right) noexcept {
    if (left == right)
        return true;
    auto next = [](std::string_view &value) noexcept {
        auto point = static_cast<unsigned char>(value.front());
        auto hex = [](unsigned char c) noexcept -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        if (point == '%' && value.size() >= 3) {
            auto high = hex(value[1]), low = hex(value[2]);
            if (high >= 0 && low >= 0) {
                point = static_cast<unsigned char>(16 * high + low);
                value.remove_prefix(3);
                auto unreserved = (point >= 'a' && point <= 'z') ||
                                  (point >= 'A' && point <= 'Z') ||
                                  (point >= '0' && point <= '9') ||
                                  point == '-' || point == '.' || point == '_' || point == '~';
                return std::pair{point, point < 0x80 && !unreserved};
            }
        }
        value.remove_prefix(1);
        return std::pair{point, false};
    };
    while (!left.empty() && !right.empty())
        if (next(left) != next(right))
            return false;
    return left.empty() && right.empty();
}
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
class Session {
  public:
    std::filesystem::path profile;
    FileLock lock;
    Handle job, process;
    std::unique_ptr<BrowserSocket> socket;
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
        auto child = create_quiet_process(executable, std::move(command));
        process = std::move(child.process);
        auto thread = std::move(child.thread);
        if (!AssignProcessToJobObject(job.get(), process.get())) {
            TerminateProcess(process.get(), 1);
            throw Error("Cannot own Edge process cleanup.");
        }
        if (ResumeThread(thread.get()) == DWORD(-1))
            throw Error("Cannot resume the retrieval browser.");
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
        socket = std::make_unique<BrowserSocket>(wide("ws://127.0.0.1:" + port_text + socket_path));
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
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cleanup ? 2 : 15);
        socket->send(std::move(encoded), cleanup, deadline);
        for (;;) {
            auto response = browser_response(socket->receive(cleanup, deadline), id);
            if (response)
                return std::move(*response);
        }
    }
};
std::filesystem::path prepare_profile(const std::filesystem::path &path) {
    auto result = std::filesystem::absolute(path).lexically_normal();
    std::filesystem::create_directories(result);
    auto attributes = GetFileAttributesW(result.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw Error("Browser profile must be a normal directory, not a directory link.");
    if (_wcsicmp(result.filename().c_str(), L"User Data") == 0)
        throw Error("Use a dedicated application profile, not personal browser User Data.");
    return result;
}
}
std::optional<boost::json::object> browser_response(std::string_view message, std::int64_t id) {
    auto storage = boost::json::make_shared_resource<boost::json::monotonic_resource>();
    auto value = boost::json::parse(message, storage);
    auto &object = value.as_object();
    auto *response_id = object.if_contains("id");
    if (!response_id || response_id->to_number<std::int64_t>() != id)
        return std::nullopt;
    if (auto *failure = object.if_contains("error"))
        throw Error("Edge command failed: " + boost::json::serialize(*failure));
    return std::move(object.at("result").as_object());
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
void resolve_browser_access(const std::filesystem::path &path, std::chrono::seconds timeout,
                            bool launcher, std::string_view url, AccessReason reason) {
    if (!interactive() && !launcher)
        throw Error("RYM browser access requires an interactive terminal or the GUI launcher.");
    auto target = site_url(url);
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
                   L" --disable-extensions --no-first-run --no-default-browser-check --new-window " +
                   quote(wide(target));
    auto child = create_quiet_process(executable, std::move(command));
    process = std::move(child.process);
    auto thread = std::move(child.thread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 1);
        throw Error("Cannot own verification browser cleanup.");
    }
    if (ResumeThread(thread.get()) == DWORD(-1))
        throw Error("Cannot resume the access browser.");
    log("If RYM shows its own restriction form, complete reCAPTCHA, click Submit, "
        "and wait for the requested page to appear before continuing. Comments and email are optional.");
    log("This window opens the requested RYM URL with the saved session. "
        "If only the normal homepage appears without an access prompt, close the window "
        "to retry retrieval.");
    if (reason == AccessReason::sign_in)
        log("RYM requested sign-in. Sign in manually in this dedicated Edge window and wait for "
            "the requested page or your signed-in homepage. Choose Remember me if offered and desired. "
            "The program will retry retrieval using the session retained inside Edge.");
    if (launcher)
        log("Complete the requested RYM access steps in Edge, then close that browser window "
            "to continue. For security checks, wait for the requested page first. Cookies are retained.");
    else
        log("Complete the requested RYM access steps in Edge, then press Enter in this terminal "
            "or close that browser window. For security checks, wait for the requested page first. "
            "The program will reuse its cookies.");
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        check_cancelled();
        if (!launcher && _kbhit()) {
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
    throw Error(reason == AccessReason::sign_in
                    ? "RYM sign-in timed out."
                    : "Normal-browser verification timed out.");
}
Browser::Browser(const std::filesystem::path &profile, Pacer &pacer, bool launcher)
    : data_(std::make_unique<Data>(std::make_shared<Session>(prepare_profile(profile), pacer))),
      launcher_(launcher) {}
Browser::Browser(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
Browser::~Browser() noexcept = default;
Browser::Browser(Browser &&) noexcept = default;
Browser &Browser::operator=(Browser &&) noexcept = default;
std::unique_ptr<Transport> Browser::fork() {
    auto result = std::unique_ptr<Browser>(new Browser(std::make_unique<Data>(data_->browser)));
    result->launcher_ = launcher_;
    return result;
}
BrowserPageState browser_page_state(std::string_view current, std::string_view requested,
                                    std::string_view ready, std::string_view html,
                                    bool expired) noexcept {
    if (challenged(html))
        return BrowserPageState::verification;
    auto access_url = current.substr(0, current.find_first_of("?#"));
    while (access_url.ends_with('/'))
        access_url.remove_suffix(1);
    if (access_url == "https://rateyourmusic.com/account/login")
        return BrowserPageState::sign_in;
    auto canonical = [](std::string_view value) noexcept {
        value = value.substr(0, value.find('#'));
        while (value.ends_with('/'))
            value.remove_suffix(1);
        return value;
    };
    if (!html.empty() && same_url_text(canonical(current), canonical(requested)) && ready == "complete" &&
        (requested.find("/charts/") == requested.npos ||
         html.find("page_charts_section_charts_item") != html.npos))
        return BrowserPageState::ready;
    return expired ? BrowserPageState::verification : BrowserPageState::loading;
}
AccessTransport::AccessTransport(std::function<std::string(std::string_view)> fetch,
                                 std::function<void(std::string_view, AccessReason)> recover)
    : fetch_(std::move(fetch)), recover_(std::move(recover)) {}
std::string AccessTransport::fetch(std::string_view url) {
    bool verified{}, signed_in{};
    for (;;) {
        try {
            check_cancelled();
            return fetch_(url);
        } catch (const VerificationRequired &error) {
            if (verified)
                throw Error(
                    "RYM page is still unusable after browser verification: " + std::string(url) +
                    ". Access may remain blocked or chart markup changed. "
                    "The previous report is preserved.");
            verified = true;
            check_cancelled();
            log(error.what());
            recover_(url, AccessReason::verification);
        } catch (const SignInRequired &error) {
            if (signed_in)
                throw Error("RYM still requests sign-in for " + std::string(url) +
                            ". Complete sign-in before closing Edge; the chart or your signed-in "
                            "homepage is acceptable. "
                            "The previous report is preserved.");
            signed_in = true;
            check_cancelled();
            log(error.what());
            recover_(url, AccessReason::sign_in);
        }
    }
}
std::string Browser::fetch(std::string_view url) {
    auto target = site_url(url);
    auto &state = *data_->browser;
    state.pacer.wait(state.restricted.get_token());
    auto navigation = state.command("Page.navigate", {{"url", target}}, data_->session);
    if (auto *failure = navigation.if_contains("errorText");
        failure && !failure->as_string().empty())
        throw Error("Cannot navigate to " + target + ": " + std::string(failure->as_string()));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::string observed_url, observed_ready;
    for (;;) {
        check_cancelled();
        if (std::chrono::steady_clock::now() >= deadline) {
            state.restricted.request_stop();
            throw VerificationRequired(
                "RYM page did not become usable within 10 seconds. Requested URL: " +
                target + ". Last page URL: " + (observed_url.empty() ? "unavailable" : observed_url) +
                ". Document state: " + (observed_ready.empty() ? "unavailable" : observed_ready) +
                ". Access restrictions, redirects, or changed markup can cause this failure.");
        }
        std::optional<boost::json::object> evaluated;
        try {
            evaluated.emplace(state.command(
                "Runtime.evaluate",
                {{"expression", "({url:location.href,ready:document.readyState,html:document."
                                "documentElement?document.documentElement.outerHTML:''})"},
                 {"returnByValue", true}},
                data_->session));
        } catch (const Error &error) {
            check_cancelled();
            if (!state.alive() || std::string_view(error.what()).find("Edge command failed:") ==
                                      std::string_view::npos)
                throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        auto *result = evaluated->if_contains("result");
        if (!result || !result->is_object() || !result->as_object().if_contains("value")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const auto &value = result->as_object().at("value").as_object();
        auto field = [&](boost::json::string_view key) {
            const auto &text = value.at(key).as_string();
            return std::string_view(text.data(), text.size());
        };
        auto current = field("url"), html = field("html"), ready = field("ready");
        observed_url.assign(current);
        observed_ready.assign(ready);
        if (current != "about:blank" && !current.starts_with("https://rateyourmusic.com/"))
            throw Error("Browser navigated outside RYM.");
        const auto status = browser_page_state(current, target, ready, html);
        if (status == BrowserPageState::sign_in) {
            state.restricted.request_stop();
            throw SignInRequired("RYM redirected " + target + " to " + std::string(current) +
                                 " and returned a sign-in page instead of the requested data.");
        }
        if (status == BrowserPageState::verification) {
            state.restricted.request_stop();
            if (!interactive() && !launcher_)
                throw VerificationRequired("RYM verification requires an interactive terminal; rerun "
                            "interactively or use --offline.");
            throw VerificationRequired("RYM security check detected at " + std::string(current) +
                                       " while retrieving " + target + ".");
        } else if (status == BrowserPageState::ready) {
            if (html.size() > 16 * 1024 * 1024)
                throw Error("HTML exceeds 16 MiB.");
            return std::string(html);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}
}
