#include "rym_crosscheck/reviewer/application.hpp"
#include "rym_crosscheck/cli/launcher.hpp"
#include "rym_crosscheck/report/report.hpp"
#include "rym_crosscheck/rym/parser.hpp"
#include <objbase.h>

#include <WebView2.h>
#include <algorithm>
#include <commctrl.h>
#include <deque>
#include <functional>
#include <memory>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl.h>

namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
constexpr UINT process_message = WM_APP + 1, io_complete = WM_APP + 2, startup_error = WM_APP + 3,
               run_update = WM_APP + 4;
constexpr wchar_t runtime_url[] =
    L"https://developer.microsoft.com/microsoft-edge/webview2/#download-section";
void check(HRESULT result, std::string_view operation) {
    if (FAILED(result))
        throw rym::Error(std::string(operation) + " (HRESULT " + std::to_string(result) + ").");
}
struct TaskFree {
    void operator()(wchar_t *value) const noexcept {
        CoTaskMemFree(value);
    }
};
using TaskString = std::unique_ptr<wchar_t, TaskFree>;
int prompt(HWND window, const wchar_t *title, const wchar_t *instruction,
           const std::wstring &content, std::initializer_list<TASKDIALOG_BUTTON> buttons) {
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = window;
    config.pszWindowTitle = title;
    config.pszMainInstruction = instruction;
    config.pszContent = content.c_str();
    config.pButtons = buttons.begin();
    config.cButtons = static_cast<UINT>(buttons.size());
    config.nDefaultButton = IDCANCEL;
    int choice = IDCANCEL;
    check(TaskDialogIndirect(&config, &choice, nullptr, nullptr), "Cannot show confirmation");
    return choice;
}
std::filesystem::path pick(HWND window, bool folder, bool report = false) {
    ComPtr<IFileOpenDialog> dialog;
    check(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&dialog)),
          "Cannot open file picker");
    DWORD flags{};
    check(dialog->GetOptions(&flags), "Cannot configure picker");
    check(dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                             (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST)),
          "Cannot configure picker");
    if (!folder) {
        COMDLG_FILTERSPEC filter{report ? L"RYM report JSON" : L"Whitelist TOML",
                                 report ? L"*.json" : L"*.toml"};
        check(dialog->SetFileTypes(1, &filter), "Cannot configure file types");
    }
    auto result = dialog->Show(window);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    check(result, "Cannot select a file or folder");
    ComPtr<IShellItem> item;
    check(dialog->GetResult(&item), "Cannot read picker selection");
    LPWSTR raw{};
    check(item->GetDisplayName(SIGDN_FILESYSPATH, &raw), "Cannot read selected path");
    TaskString path(raw);
    return std::filesystem::path(path.get());
}
std::vector<std::string> selected(const boost::json::value &value) {
    const auto &array = value.as_array();
    if (array.size() > 1000000)
        throw rym::Error("Too many review selections.");
    std::vector<std::string> result;
    result.reserve(array.size());
    for (const auto &item : array) {
        const auto &str = item.as_string();
        result.emplace_back(str.data(), str.size());
    }
    return result;
}
class Reviewer : public std::enable_shared_from_this<Reviewer> {
    struct Completion {
        std::optional<rym::SaveResult> value;
        std::string error;
        std::wstring payload;
        bool saved{};
        std::function<void()> after;
    };
    rym::ReportStore store_;
    std::shared_ptr<rym::ReviewDocument> document_;
    HWND window_{};
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    bool alive_{true}, ready_{}, busy_{};
    std::deque<boost::json::object> messages_;
    std::jthread worker_;
    std::mutex completion_mutex_;
    std::optional<Completion> completion_;
    std::pair<std::int64_t, std::int64_t> observed_{};
    std::string startup_failure_;
    std::string save_warning_, warning_generation_;
    std::wstring trusted_source_;
    bool initial_navigation_{true};
    std::string screen_{"home"};
    bool requested_results_{}, close_after_run_{}, modal_{};
    std::unique_ptr<rym::RunSupervisor> run_;
    rym::ComparisonOptions run_options_;
    std::chrono::steady_clock::time_point started_;
    std::optional<boost::json::object> progress_;
    std::optional<rym::RunOutcome> outcome_;
    std::function<void()> generic_completion_;
    struct Modal {
        Reviewer &owner;
        explicit Modal(Reviewer &value) noexcept : owner(value) {
            owner.modal_ = true;
        }
        ~Modal() noexcept {
            owner.modal_ = false;
            PostMessageW(owner.window_, process_message, 0, 0);
            if (owner.run_)
                PostMessageW(owner.window_, run_update, 0, 0);
        }
    };
    int decision(const wchar_t *title, const wchar_t *instruction, const std::wstring &content,
                 std::initializer_list<TASKDIALOG_BUTTON> buttons) {
        Modal modal{*this};
        return prompt(window_, title, instruction, content, buttons);
    }
    std::filesystem::path choose(bool folder, bool report = false) {
        Modal modal{*this};
        return pick(window_, folder, report);
    }
    void page(std::string name) {
        screen_ = std::move(name);
        send({{"type", "screen"}, {"name", screen_}});
    }
    void work(std::function<std::function<void()>()> function) {
        if (busy_)
            throw rym::Error("Please wait for the current file operation.");
        busy_ = true;
        send({{"type", "busy"}});
        worker_ = std::jthread([this, function = std::move(function)] {
            std::function<void()> done;
            try {
                done = function();
            } catch (const std::exception &failure) {
                auto text = std::string(failure.what());
                done = [this, text = std::move(text)] { error(text); };
            }
            {
                std::lock_guard lock(completion_mutex_);
                generic_completion_ = std::move(done);
            }
            PostMessageW(window_, io_complete, 0, 0);
        });
    }
    void send(boost::json::object value) {
        if (!ready_ || !webview_)
            return;
        auto json = rym::wide(boost::json::serialize(value));
        check(webview_->PostWebMessageAsJson(json.c_str()), "Cannot update the review interface");
    }
    void state(std::wstring payload, std::string message = {}) {
        if (!ready_ || !webview_ || payload.empty())
            return;
        check(webview_->PostWebMessageAsJson(payload.c_str()),
              "Cannot update the review interface");
        if (!message.empty())
            send({{"type", "notice"}, {"message", std::move(message)}});
    }
    void error(std::string message) {
        if (ready_)
            send({{"type", "error"}, {"message", std::move(message)}, {"busy", busy_}});
        else
            MessageBoxW(window_, rym::wide(message).c_str(), L"RYM music review",
                        MB_OK | MB_ICONERROR);
    }
    void job(std::function<rym::SaveResult()> function, bool saved,
             std::function<void()> after = {}) {
        if (busy_)
            throw rym::Error("A review operation is already in progress.");
        busy_ = true;
        send({{"type", "busy"}});
        worker_ = std::jthread([this, function = std::move(function), saved,
                                after = std::move(after)] {
            Completion result;
            result.saved = saved;
            result.after = after;
            try {
                result.value = function();
                result.payload = rym::wide(boost::json::serialize(boost::json::object{
                    {"type", "state"}, {"data", rym::review_json(result.value->document)}}));
            } catch (const std::exception &failure) {
                result.error =
                    result.value
                        ? "Data was saved or loaded, but the interface could not be refreshed: " +
                              std::string(failure.what())
                        : failure.what();
            }
            {
                std::lock_guard guard(completion_mutex_);
                completion_ = std::move(result);
            }
            PostMessageW(window_, io_complete, 0, 0);
        });
    }
    void reload() {
        auto paths = store_.paths();
        page("results");
        job([paths] { return rym::ReportStore(paths).available(); }, false);
    }
    void changed() {
        if (!ready_)
            return;
        send({{"type", "new_report"}});
    }
    bool permitted_url(const std::string &url) const {
        if (!document_ || rym::song_url(url) != url)
            return false;
        if (document_->report)
            for (const auto &row : document_->report->songs)
                if (row.song.url == url)
                    return true;
        return std::ranges::any_of(document_->whitelist.entries(),
                                   [&](const auto &entry) { return entry.url == url; });
    }
    void route(const std::string &target) {
        if (target == "close")
            DestroyWindow(window_);
        else if (target == "home")
            page("home");
        else if (target == "results")
            reload();
        else if (target == "form") {
            page("form");
            auto paths = store_.paths();
            work([this, paths] {
                auto value = rym::default_options(paths);
                std::string warning;
                try {
                    value = rym::load_settings(paths);
                } catch (const std::exception &failure) {
                    warning = "Remembered settings could not be loaded; defaults restored: " +
                              std::string(failure.what());
                }
                auto json = rym::options_json(value);
                return [this, json = std::move(json), warning = std::move(warning)] {
                    send({{"type", "options"}, {"data", json}});
                    if (!warning.empty())
                        error(warning);
                };
            });
        } else
            throw rym::Error("Unknown screen.");
    }
    void guard(std::vector<std::string> add, std::vector<std::string> remove,
               std::function<void()> next) {
        if (screen_ != "results" || !document_) {
            next();
            return;
        }
        if (!document_->whitelist_valid) {
            if (!add.empty() || !remove.empty())
                throw rym::Error("Load a valid whitelist before editing exclusions.");
            next();
            return;
        }
        auto baseline = document_;
        auto paths = store_.paths();
        work([this, baseline, paths, add = std::move(add), remove = std::move(remove),
              next = std::move(next)] {
            auto proposed =
                std::make_shared<rym::Whitelist>(rym::proposed_whitelist(*baseline, add, remove));
            bool differs;
            std::string warning;
            try {
                differs = rym::ReportStore(paths).differs(*proposed);
            } catch (const std::exception &failure) {
                differs = true;
                warning = "\n\nThe local whitelist cannot be read: " + std::string(failure.what());
            }
            return [this, paths, proposed, differs, warning = std::move(warning), next] {
                if (!differs) {
                    next();
                    return;
                }
                auto choice = decision(
                    L"Whitelist differs", L"Replace local with the displayed whitelist?",
                    L"The displayed whitelist, including your selections, differs from disk. "
                    L"Replace local writes exactly the displayed set and can remove external "
                    L"edits. Discard leaves disk untouched." +
                        rym::wide(warning),
                    {{100, L"Replace local"}, {101, L"Discard"}, {IDCANCEL, L"Cancel"}});
                if (choice == IDCANCEL)
                    return;
                if (choice == 101) {
                    job([paths] { return rym::ReportStore(paths).available(); }, false, next);
                    return;
                }
                job([paths, proposed] { return rym::ReportStore(paths).replace(*proposed); }, true,
                    next);
            };
        });
    }
    void import_file(bool report, std::vector<std::string> add, std::vector<std::string> remove) {
        auto source = choose(false, report);
        if (source.empty())
            return;
        auto paths = store_.paths();
        std::error_code ec;
        if (std::filesystem::equivalent(source, report ? paths.report : paths.whitelist, ec)) {
            send({{"type", "notice"},
                  {"message", "That is already the active file. No changes made."}});
            return;
        }
        work([this, paths, source, report, add = std::move(add), remove = std::move(remove)] {
            auto input =
                std::make_shared<rym::ImportData>(rym::inspect_import(paths, source, report));
            return [this, paths, source, report, input, add, remove] {
                auto text =
                    source.filename().native() + L"\n" + std::to_wstring(input->count()) +
                    (report ? L" chart songs" : L" exclusions") +
                    L"\n\nThe source remains unchanged. Only the selected active file is replaced.";
                if (decision(L"Load validated file", L"Replace the active file?", text,
                             {{100, L"Replace"}, {IDCANCEL, L"Cancel"}}) != 100)
                    return;
                guard(add, remove, [this, paths, input] {
                    job(
                        [paths, input] {
                            rym::ReportStore store(paths);
                            store.initialize(false);
                            return store.import_file(*input);
                        },
                        true);
                });
            };
        });
    }
    void start_run(rym::ComparisonOptions options) {
        run_options_ = std::move(options);
        close_after_run_ = false;
        started_ = std::chrono::steady_clock::now();
        page("running");
        send({{"type", "progress"}, {"stage", "starting"}, {"message", "Starting comparison"}});
        auto weak = weak_from_this();
        run_ = std::make_unique<rym::RunSupervisor>(
            store_.paths().cli, rym::comparison_arguments(run_options_),
            [weak](boost::json::object value) {
                if (auto self = weak.lock()) {
                    {
                        std::lock_guard lock(self->completion_mutex_);
                        self->progress_ = std::move(value);
                    }
                    PostMessageW(self->window_, run_update, 0, 0);
                }
            },
            [weak](rym::RunOutcome value) {
                if (auto self = weak.lock()) {
                    {
                        std::lock_guard lock(self->completion_mutex_);
                        self->outcome_ = std::move(value);
                    }
                    PostMessageW(self->window_, run_update, 0, 0);
                }
            });
    }
    void dispatch(boost::json::object message) {
        const auto &type = message.at("type").as_string();
        if (type == "ready") {
            ready_ = true;
            page("home");
            if (requested_results_) {
                requested_results_ = false;
                reload();
            }
            return;
        }
        if (modal_)
            return;
        if (type == "cancel_run") {
            if (run_) {
                run_->cancel();
                send({{"type", "progress"},
                      {"stage", "stopping"},
                      {"message", "Stopping; waiting for CLI cleanup..."}});
            }
            return;
        }
        if (run_)
            throw rym::Error("Wait for the comparison to finish or cancel it.");
        if (type == "open_song") {
            auto url = std::string(message.at("url").as_string());
            if (!permitted_url(url))
                throw rym::Error("This link is not part of the current report or whitelist.");
            auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(
                window_, L"open", rym::wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (opened <= 32)
                throw rym::Error("Cannot open the song in your browser.");
            return;
        }
        if (busy_)
            throw rym::Error("Please wait for the current save or reload to finish.");
        auto selections = [&](const char *key) {
            auto *value = message.if_contains(key);
            return value ? selected(*value) : std::vector<std::string>{};
        };
        if (type == "navigate") {
            auto target = std::string(message.at("target").as_string());
            guard(selections("add"), selections("remove"), [this, target] { route(target); });
            return;
        }
        if (type == "pick_music") {
            auto path = choose(true);
            if (!path.empty())
                send({{"type", "music_folder"}, {"path", rym::path_text(path)}});
            return;
        }
        if (type == "retry") {
            page("form");
            auto options = run_options_;
            options.offline = false;
            options.clear = false;
            send({{"type", "options"}, {"data", rym::options_json(options)}});
            return;
        }
        if (type == "start") {
            auto value = rym::parse_options(message.at("options").as_object(), store_.paths());
            work([this, value = std::move(value)]() mutable {
                value = rym::validate_options(std::move(value));
                return [this, value = std::move(value)]() mutable { start_run(std::move(value)); };
            });
            return;
        }
        if (type == "load_report" || type == "load_whitelist") {
            import_file(type == "load_report", selections("add"), selections("remove"));
            return;
        }
        if (type == "save") {
            if (!document_ || screen_ != "results")
                throw rym::Error("Open Results before saving.");
            auto add = selected(message.at("add")), remove = selected(message.at("remove"));
            auto paths = store_.paths();
            auto baseline = document_;
            job(
                [paths, baseline, add = std::move(add), remove = std::move(remove)] {
                    return rym::ReportStore(paths).apply(*baseline, add, remove);
                },
                true);
            return;
        }
        throw rym::Error("Unsupported reviewer message.");
    }
    void setup(ICoreWebView2Controller *controller) {
        controller_ = controller;
        check(controller_->get_CoreWebView2(&webview_), "Cannot initialize the review page");
        ComPtr<ICoreWebView2Settings> settings;
        check(webview_->get_Settings(&settings), "Cannot configure the review page");
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreHostObjectsAllowed(FALSE);
        ComPtr<ICoreWebView2Settings3> accelerators;
        if (SUCCEEDED(settings.As(&accelerators)))
            accelerators->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        auto weak = weak_from_this();
        EventRegistrationToken token{};
        check(webview_->add_WebMessageReceived(
                  Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                      [weak](ICoreWebView2 *,
                             ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                          if (auto self = weak.lock(); self && self->alive_) {
                              try {
                                  LPWSTR raw{};
                                  check(args->get_Source(&raw), "Cannot identify the review page");
                                  TaskString source(raw);
                                  if (std::wstring_view(source.get()) != L"about:blank" &&
                                      std::wstring_view(source.get()) != self->trusted_source_)
                                      return S_OK;
                                  raw = nullptr;
                                  check(args->get_WebMessageAsJson(&raw),
                                        "Cannot read a reviewer message");
                                  TaskString json(raw);
                                  auto encoded = rym::utf8(json.get());
                                  if (encoded.size() > 16 * 1024 * 1024)
                                      return E_INVALIDARG;
                                  self->messages_.push_back(
                                      boost::json::parse(encoded).as_object());
                                  PostMessageW(self->window_, process_message, 0, 0);
                              } catch (...) {
                                  return E_INVALIDARG;
                              }
                          }
                          return S_OK;
                      })
                      .Get(),
                  &token),
              "Cannot connect reviewer actions");
        check(webview_->add_NavigationStarting(
                  Callback<ICoreWebView2NavigationStartingEventHandler>(
                      [weak](ICoreWebView2 *,
                             ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                          LPWSTR raw{};
                          if (auto self = weak.lock();
                              self && self->alive_ && SUCCEEDED(args->get_Uri(&raw))) {
                              TaskString uri(raw);
                              auto target = std::wstring_view(uri.get());
                              if (self->initial_navigation_ &&
                                  (target == L"about:blank" ||
                                   target.starts_with(L"data:text/html;"))) {
                                  self->trusted_source_ = target;
                                  self->initial_navigation_ = false;
                              } else
                                  args->put_Cancel(TRUE);
                          } else
                              args->put_Cancel(TRUE);
                          return S_OK;
                      })
                      .Get(),
                  &token),
              "Cannot restrict review navigation");
        check(
            webview_->add_NewWindowRequested(
                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                    [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                        args->put_Handled(TRUE);
                        return S_OK;
                    })
                    .Get(),
                &token),
            "Cannot restrict new review windows");
        check(webview_->add_PermissionRequested(
                  Callback<ICoreWebView2PermissionRequestedEventHandler>(
                      [](ICoreWebView2 *,
                         ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
                          args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                          return S_OK;
                      })
                      .Get(),
                  &token),
              "Cannot restrict review permissions");
        auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA);
        auto bytes = resource ? SizeofResource(nullptr, resource) : 0;
        auto loaded = resource ? LoadResource(nullptr, resource) : nullptr;
        auto *html = loaded ? static_cast<const char *>(LockResource(loaded)) : nullptr;
        if (!html || !bytes || bytes >= 2 * 1024 * 1024)
            throw rym::Error("The embedded review interface is unavailable.");
        auto content = rym::wide({html, bytes});
        check(webview_->NavigateToString(content.c_str()), "Cannot display the review interface");
        controller_->put_IsVisible(!IsIconic(window_));
        RECT bounds{};
        GetClientRect(window_, &bounds);
        controller_->put_Bounds(bounds);
    }

  public:
    explicit Reviewer(rym::PortablePaths paths, bool show_results)
        : store_(std::move(paths)), requested_results_(show_results) {}
    ~Reviewer() noexcept {
        run_.reset();
        if (worker_.joinable())
            worker_.join();
    }
    void start(HWND window) {
        window_ = window;
        auto base = rym::environment(L"LOCALAPPDATA");
        if (base.empty())
            throw rym::Error("LOCALAPPDATA is unavailable for the WebView2 reviewer profile.");
        auto profile = std::filesystem::path(base) / L"rym-crosscheck/reviewer" /
                       rym::installation_id(store_.paths());
        auto weak = weak_from_this();
        check(
            CreateCoreWebView2EnvironmentWithOptions(
                nullptr, profile.c_str(), nullptr,
                Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                    [weak](HRESULT status, ICoreWebView2Environment *environment) -> HRESULT {
                        if (auto self = weak.lock(); self && self->alive_) {
                            try {
                                check(status, "Cannot start WebView2; install or update the "
                                              "Evergreen Runtime");
                                if (!environment)
                                    throw rym::Error("WebView2 did not provide an environment.");
                                self->environment_ = environment;
                                check(
                                    environment->CreateCoreWebView2Controller(
                                        self->window_,
                                        Callback<
                                            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                            [weak](HRESULT created,
                                                   ICoreWebView2Controller *controller) -> HRESULT {
                                                if (auto app = weak.lock(); app && app->alive_) {
                                                    try {
                                                        check(
                                                            created,
                                                            "Cannot create the WebView2 reviewer");
                                                        if (!controller)
                                                            throw rym::Error(
                                                                "WebView2 did not provide a "
                                                                "controller.");
                                                        app->setup(controller);
                                                    } catch (const std::exception &failure) {
                                                        app->startup_failure_ = failure.what();
                                                        PostMessageW(app->window_, startup_error, 0,
                                                                     0);
                                                    }
                                                }
                                                return S_OK;
                                            })
                                            .Get()),
                                    "Cannot create the review window");
                            } catch (const std::exception &failure) {
                                self->startup_failure_ = failure.what();
                                PostMessageW(self->window_, startup_error, 0, 0);
                            }
                        }
                        return S_OK;
                    })
                    .Get()),
            "Cannot initialize WebView2");
        SetTimer(window_, 1, 1000, nullptr);
    }
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_SIZE:
            if (controller_) {
                auto visible = wparam != SIZE_MINIMIZED;
                controller_->put_IsVisible(visible);
                ComPtr<ICoreWebView2_19> memory;
                if (SUCCEEDED(webview_.As(&memory)))
                    memory->put_MemoryUsageTargetLevel(
                        visible ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL
                                : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
                RECT bounds{};
                GetClientRect(window_, &bounds);
                controller_->put_Bounds(bounds);
            }
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {720, 500};
            return 0;
        case WM_COPYDATA:
            if (auto *copy = reinterpret_cast<COPYDATASTRUCT *>(lparam);
                copy && (copy->dwData == 1 || copy->dwData == 2) && !copy->cbData) {
                if (IsIconic(window_))
                    ShowWindow(window_, SW_RESTORE);
                SetForegroundWindow(window_);
                if (copy->dwData == 2 && !run_) {
                    if (ready_ && !busy_ && !modal_)
                        send({{"type", "navigate_request"}, {"target", "results"}});
                    else
                        requested_results_ = true;
                }
                return TRUE;
            }
            return FALSE;
        case WM_TIMER:
            if (run_) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::steady_clock::now() - started_)
                                   .count();
                send({{"type", "elapsed"}, {"seconds", elapsed}});
            } else if (screen_ == "results" && !busy_) {
                std::error_code ec;
                auto stamp = std::filesystem::last_write_time(store_.paths().report, ec);
                auto current = ec ? 0 : stamp.time_since_epoch().count();
                if (current != observed_.first) {
                    observed_.first = current;
                    changed();
                }
            }
            return 0;
        case process_message:
            while (!messages_.empty()) {
                auto action = std::move(messages_.front());
                messages_.pop_front();
                try {
                    dispatch(std::move(action));
                } catch (const std::exception &failure) {
                    error(failure.what());
                }
            }
            if (requested_results_ && ready_ && !busy_ && !run_ && !modal_) {
                requested_results_ = false;
                send({{"type", "navigate_request"}, {"target", "results"}});
            }
            return 0;
        case io_complete: {
            std::optional<Completion> result;
            std::function<void()> generic;
            {
                std::lock_guard guard(completion_mutex_);
                generic = std::move(generic_completion_);
                result.swap(completion_);
            }
            if (generic) {
                busy_ = false;
                send({{"type", "idle"}});
                generic();
                if (requested_results_ && !busy_ && !run_) {
                    requested_results_ = false;
                    send({{"type", "navigate_request"}, {"target", "results"}});
                }
                return 0;
            }
            if (!result)
                return 0;
            busy_ = false;
            if (!result->error.empty()) {
                error(result->error);
                return 0;
            }
            document_ = std::make_shared<rym::ReviewDocument>(std::move(result->value->document));
            observed_ = document_->revision;
            auto warning = result->value->warning;
            auto generation = document_->report ? document_->report->generation : std::string{};
            if (result->saved) {
                save_warning_ = warning;
                warning_generation_ = generation;
            } else if (generation != warning_generation_)
                save_warning_.clear();
            state(std::move(result->payload),
                  !warning.empty()        ? warning
                  : save_warning_.empty() ? (result->saved ? "Changes saved." : "Report loaded.")
                                          : save_warning_);
            send({{"type", "idle"}});
            if (result->after) {
                if (!warning.empty())
                    MessageBoxW(window_, rym::wide(warning).c_str(), L"Whitelist update",
                                MB_OK | MB_ICONWARNING);
                result->after();
                return 0;
            }
            if (requested_results_ && !busy_ && !run_) {
                requested_results_ = false;
                send({{"type", "navigate_request"}, {"target", "results"}});
            }
            return 0;
        }
        case run_update: {
            if (modal_)
                return 0;
            std::optional<boost::json::object> progress;
            std::optional<rym::RunOutcome> outcome;
            {
                std::lock_guard lock(completion_mutex_);
                progress = std::move(progress_);
                progress_.reset();
                outcome = std::move(outcome_);
                outcome_.reset();
            }
            if (progress) {
                (*progress)["type"] = "progress";
                send(std::move(*progress));
            }
            if (outcome) {
                run_.reset();
                if (outcome->exit_code == 0 || outcome->exit_code == 2) {
                    auto paths = store_.paths();
                    auto options = run_options_;
                    page("results");
                    job(
                        [paths, options] {
                            auto result = rym::ReportStore(paths).available();
                            try {
                                rym::save_settings(paths, options);
                            } catch (const std::exception &failure) {
                                result.warning +=
                                    " Comparison completed, but settings could not be saved: " +
                                    std::string(failure.what());
                            }
                            return result;
                        },
                        false,
                        close_after_run_ ? std::function<void()>([this] { DestroyWindow(window_); })
                                         : std::function<void()>{});
                } else {
                    if (close_after_run_) {
                        DestroyWindow(window_);
                        return 0;
                    }
                    page("outcome");
                    send({{"type", "outcome"},
                          {"cancelled", outcome->cancellation_requested},
                          {"diagnostics", outcome->diagnostics}});
                }
            }
            return 0;
        }
        case startup_error: {
            auto text = rym::wide(startup_failure_ + "\n\nYour report and whitelist are preserved. "
                                                     "Open the official WebView2 download page?");
            if (MessageBoxW(window_, text.c_str(), L"WebView2 could not start",
                            MB_YESNO | MB_ICONERROR | MB_DEFBUTTON2) == IDYES)
                ShellExecuteW(window_, L"open", runtime_url, nullptr, nullptr, SW_SHOWNORMAL);
            DestroyWindow(window_);
            return 0;
        }
        case WM_CLOSE:
            if (modal_)
                return 0;
            if (run_) {
                auto choice = decision(
                    L"Comparison running", L"Stop the comparison and close?",
                    L"Stopping requests cancellation and waits for the CLI to release files and "
                    L"close its browser processes.",
                    {{100, L"Stop and close"}, {101, L"Keep running"}, {IDCANCEL, L"Cancel"}});
                if (choice == 100) {
                    close_after_run_ = true;
                    run_->cancel();
                    send({{"type", "progress"},
                          {"stage", "stopping"},
                          {"message", "Stopping; waiting for cleanup before closing..."}});
                }
                return 0;
            }
            if (busy_) {
                error("Please wait for the current file operation before closing.");
                return 0;
            }
            if (screen_ == "results" && ready_) {
                send({{"type", "navigate_request"}, {"target", "close"}});
                return 0;
            }
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            alive_ = false;
            ready_ = false;
            KillTimer(window_, 1);
            if (controller_)
                controller_->Close();
            webview_.Reset();
            controller_.Reset();
            environment_.Reset();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wparam, lparam);
        }
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam) noexcept {
        auto *app = reinterpret_cast<Reviewer *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app =
                static_cast<Reviewer *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        try {
            return app ? app->handle(message, wparam, lparam)
                       : DefWindowProcW(window, message, wparam, lparam);
        } catch (const std::exception &failure) {
            MessageBoxW(window, rym::wide(failure.what()).c_str(), L"RYM music review",
                        MB_OK | MB_ICONERROR);
            return 0;
        }
    }
};
}
int rym::reviewer_application(HINSTANCE instance) {
    try {
        check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "Cannot initialize the reviewer");
        struct Apartment {
            ~Apartment() noexcept {
                CoUninitialize();
            }
        } apartment;
        auto directory = rym::executable_directory();
        auto paths = rym::PortablePaths::from_directory(directory);
        int count{};
        auto raw_args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!raw_args)
            throw rym::Error("Cannot read viewer arguments.");
        struct Arguments {
            LPWSTR *value;
            ~Arguments() noexcept {
                LocalFree(value);
            }
        } args{raw_args};
        bool show_results{};
        for (int i = 1; i < count; ++i) {
            if (std::wstring_view(raw_args[i]) == L"--show-results")
                show_results = true;
            else
                throw rym::Error("The viewer accepts only --show-results.");
        }
        auto id = rym::installation_id(paths);
        auto class_name = L"RYMCrosscheckReviewer-" + id;
        auto mutex_name = L"Local\\rym-crosscheck-reviewer-" + id;
        rym::Handle singleton(CreateMutexW(nullptr, TRUE, mutex_name.c_str()));
        if (!singleton)
            throw rym::Error("Cannot create the reviewer instance lock.");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            for (int attempt = 0; attempt < 50; ++attempt) {
                if (auto window = FindWindowW(class_name.c_str(), nullptr)) {
                    COPYDATASTRUCT notification{show_results ? 2u : 1u, 0, nullptr};
                    DWORD_PTR result{};
                    SendMessageTimeoutW(window, WM_COPYDATA, 0,
                                        reinterpret_cast<LPARAM>(&notification), SMTO_ABORTIFHUNG,
                                        3000, &result);
                    return 0;
                }
                Sleep(100);
            }
            throw rym::Error("The reviewer is already starting. Try again shortly.");
        }
        LPWSTR raw_version{};
        auto available = GetAvailableCoreWebView2BrowserVersionString(nullptr, &raw_version);
        TaskString version(raw_version);
        if (FAILED(available) || !version) {
            if (MessageBoxW(nullptr,
                            L"Microsoft Edge WebView2 Evergreen Runtime is required to display "
                            L"this report. Open the official download page?\n\nYour report and "
                            L"whitelist are preserved.",
                            L"WebView2 Runtime required",
                            MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2) == IDYES)
                ShellExecuteW(nullptr, L"open", runtime_url, nullptr, nullptr, SW_SHOWNORMAL);
            return 1;
        }
        auto app = std::make_shared<Reviewer>(std::move(paths), show_results);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = Reviewer::procedure;
        window_class.hInstance = instance;
        window_class.lpszClassName = class_name.c_str();
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassExW(&window_class))
            throw rym::Error("Cannot register the review window.");
        auto window = CreateWindowExW(0, class_name.c_str(), L"RYM music review",
                                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860,
                                      nullptr, nullptr, instance, app.get());
        if (!window)
            throw rym::Error("Cannot create the review window.");
        struct WindowOwner {
            HWND value;
            ~WindowOwner() noexcept {
                if (IsWindow(value))
                    DestroyWindow(value);
            }
        } owner{window};
        ShowWindow(window, SW_SHOWMAXIMIZED);
        // A launcher's startup show state can override the first ShowWindow call.
        if (!IsZoomed(window) || IsIconic(window))
            ShowWindow(window, SW_SHOWMAXIMIZED);
        UpdateWindow(window);
        app->start(window);
        MSG message{};
        int received{};
        while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return received < 0 ? 1 : 0;
    } catch (const std::exception &failure) {
        MessageBoxW(nullptr, rym::wide(failure.what()).c_str(), L"RYM music review",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
}
