#include "rym_crosscheck/cli/application.hpp"
#include "rym_crosscheck/cli/launcher.hpp"
#include "rym_crosscheck/storage/cache.hpp"
#include <condition_variable>
#include <future>
#include <iostream>
#include <shellapi.h>

namespace {
void require(bool value, std::string_view message) {
    if (!value)
        throw rym::Error(std::string(message));
}
template <class F> void rejects(F function) {
    bool failed{};
    try {
        function();
    } catch (const std::exception &) {
        failed = true;
    }
    require(failed, "Expected launcher rejection");
}
struct Temporary {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 (L"rym-launcher-test-" + std::to_wstring(GetCurrentProcessId()) +
                                  L"-" + std::to_wstring(GetTickCount64()));
    Temporary() {
        std::filesystem::create_directories(path);
    }
    ~Temporary() noexcept {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
}
int launcher_child(int argc, char **argv) {
    rym::install_cancellation();
    std::uint64_t events{}, cancel{};
    std::string id, pid_file;
    for (int i = 3; i + 1 < argc; i += 2) {
        auto key = std::string_view(argv[i]);
        if (key == "--launcher-events")
            events = std::stoull(argv[i + 1]);
        if (key == "--launcher-cancel")
            cancel = std::stoull(argv[i + 1]);
        if (key == "--launcher-id")
            id = argv[i + 1];
        if (key == "--pid-file")
            pid_file = argv[i + 1];
    }
    if (GetConsoleWindow() || rym::interactive())
        return 9;
    rym::LauncherChannel channel(events, cancel, id);
    if (!pid_file.empty())
        rym::atomic_write(rym::wide(pid_file), std::to_string(GetCurrentProcessId()));
    channel.emit("acquiring", {{"top", 200}, {"chart_songs", 40}});
    channel.emit("verification", {{"message", "Close the verification window to continue"}});
    std::jthread local_progress([&] { channel.emit({}, {{"local_files", 575}, {"cached", 0}}); });
    local_progress.join();
    std::cout << std::string(300000, 'x');
    std::cout.flush();
    if (std::string_view(argv[2]) == "wait") {
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!rym::cancelled && std::chrono::steady_clock::now() < until)
            Sleep(10);
        return rym::cancelled ? 1 : 8;
    }
    channel.emit("complete", {{"exit_code", 2}});
    return 2;
}
int launcher_parent(std::string_view destination) {
    auto self = rym::executable_directory() / "rym-tests.exe";
    rym::RunSupervisor run(
        self, {"--launcher-child", "wait", "--pid-file", std::string(destination)}, [](auto) {},
        [](auto) {});
    Sleep(20000);
    return 0;
}
void launcher_tests() {
    Temporary temporary;
    auto paths = rym::PortablePaths::from_directory(temporary.path);
    require(paths.viewer == temporary.path / "missing-report.exe" &&
                paths.cli == temporary.path / "bin/rym-crosscheck.exe" &&
                paths.report == temporary.path / "data/missing.json" &&
                paths.cache == temporary.path / "data/cache",
            "Portable installation paths");
    auto options = rym::default_options(paths);
    require(options.top == 200 && options.web_workers == 4 && options.local_workers >= 1 &&
                options.local_workers <= 8 && options.delay == 1000 &&
                options.verification == 300 && options.artist == "0.90" && options.title == "0.85",
            "Shared defaults");
    options.music = rym::path_text(temporary.path);
    options.offline = true;
    options.clear = true;
    options = rym::validate_options(options);
    rym::save_settings(paths, options);
    auto restored = rym::load_settings(paths);
    require(restored.music == options.music && restored.cache == options.cache &&
                !restored.offline && !restored.clear,
            "Successful settings round trip resets transient options");
    auto invalid = options;
    invalid.music.clear();
    rejects([&] { rym::validate_options(invalid); });
    invalid = options;
    invalid.web_workers = 9;
    rejects([&] { rym::validate_options(invalid); });
    invalid = options;
    invalid.artist = "1.01";
    rejects([&] { rym::validate_options(invalid); });
    invalid = options;
    invalid.verification = 0;
    rejects([&] { rym::validate_options(invalid); });
    std::vector<std::string> arguments{"", R"(C:\music path\)", R"(a\"quoted"title)", "Café 日本語",
                                       "--no-open"};
    auto command = rym::windows_command(L"C:\\program path\\viewer.exe", arguments);
    int count{};
    auto parsed = CommandLineToArgvW(command.c_str(), &count);
    require(parsed && count == arguments.size() + 1, "Quoted argument count");
    for (std::size_t i = 0; i < arguments.size(); ++i)
        require(rym::utf8(parsed[i + 1]) == arguments[i], "Windows argument quoting round trip");
    LocalFree(parsed);
    rym::ReportStore store(paths);
    require(!store.available().document.report && !std::filesystem::exists(paths.whitelist),
            "Empty results do not initialize or migrate files");
    {
        rym::RunLock active(paths);
        auto other = std::async(std::launch::async, [&] {
            return rym::application(
                {"rym-crosscheck", "--music-dir", options.music, "--offline", "--no-open"},
                temporary.path);
        });
        require(other.get() == 1 && !std::filesystem::exists(paths.cache) &&
                    !std::filesystem::exists(paths.whitelist),
                "Concurrent comparison is rejected before data writes or cache clearing");
    }
    store.initialize();
    rym::install_cancellation();
    auto canonical = rym::Whitelist::parse("songs=[{artist='Artist',title='Title'},{url='https://"
                                           "rateyourmusic.com/song/artist/example/'}]");
    store.replace(canonical);
    auto original = rym::read_file(paths.whitelist);
    auto entries = canonical.entries();
    std::reverse(entries.begin(), entries.end());
    require(!store.differs(rym::Whitelist(entries)),
            "Whitelist identity ignores entry order and formatting");
    require(rym::read_file(paths.whitelist) == original, "Difference checks do not write");
    std::filesystem::path self;
    {
        std::wstring module(32768, L'\0');
        auto n = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        module.resize(n);
        self = module;
    }
    auto execute = [&](std::string mode, bool cancellation) {
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<rym::RunOutcome> outcome;
        bool verification{}, counts{};
        rym::RunSupervisor run(
            self, {"--launcher-child", mode},
            [&](boost::json::object event) {
                std::lock_guard lock(mutex);
                auto *local_count = event.if_contains("local_files");
                if (local_count && local_count->to_number<int>() == 575 &&
                    event.at("chart_songs").to_number<int>() == 40 &&
                    event.at("top").to_number<int>() == 200)
                    counts = true;
                if (event.at("stage").as_string() == "verification" && counts)
                    verification = true;
                changed.notify_all();
            },
            [&](rym::RunOutcome result) {
                std::lock_guard lock(mutex);
                outcome = std::move(result);
                changed.notify_all();
            });
        std::unique_lock lock(mutex);
        if (cancellation) {
            require(changed.wait_for(lock, std::chrono::seconds(10),
                                     [&] { return verification || outcome.has_value(); }),
                    "Hidden child progress arrives");
            run.cancel();
        }
        require(
            changed.wait_for(lock, std::chrono::seconds(15), [&] { return outcome.has_value(); }),
            "Hidden child exits without GUI waits");
        if (outcome->exit_code != (cancellation ? 1 : 2))
            throw rym::Error("Hidden child exit " + std::to_string(outcome->exit_code) + ": " +
                             outcome->diagnostics.substr(0, 500));
        require(outcome->cancellation_requested == cancellation,
                "Cooperative cancellation preserved");
        require(counts, "Chart and local counts survive coalescing and worker updates");
        require(outcome->diagnostics.size() <= 256 * 1024 && !outcome->diagnostics.empty(),
                "Diagnostic capture is drained and bounded");
    };
    execute("complete", false);
    execute("wait", true);
    DWORD before{}, after{};
    GetProcessHandleCount(GetCurrentProcess(), &before);
    for (int i = 0; i < 3; ++i) {
        execute("complete", false);
        execute("wait", true);
    }
    require(GetProcessHandleCount(GetCurrentProcess(), &after) && after <= before,
            "Repeated hidden comparisons release handles");
    {
        auto pid_file = temporary.path / "supervised-child.txt";
        auto parent = rym::create_quiet_process(
            self, rym::windows_command(self, {"--launcher-parent", rym::path_text(pid_file)}));
        require(ResumeThread(parent.thread.get()) != DWORD(-1),
                "Start unexpected-termination fixture");
        struct Cleanup {
            HANDLE process;
            ~Cleanup() noexcept {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 5000);
            }
        } cleanup{parent.process.get()};
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!std::filesystem::exists(pid_file) && std::chrono::steady_clock::now() < until)
            Sleep(20);
        require(std::filesystem::exists(pid_file), "Supervised child starts");
        auto pid = static_cast<DWORD>(std::stoul(rym::read_file(pid_file)));
        rym::Handle child(OpenProcess(SYNCHRONIZE, FALSE, pid));
        require(child && TerminateProcess(parent.process.get(), 1),
                "Simulate unexpected launcher termination");
        require(WaitForSingleObject(child.get(), 5000) == WAIT_OBJECT_0,
                "Owned job prevents orphaned comparison children");
    }
    {
        auto cli = self.parent_path() / "portable/bin/rym-crosscheck.exe";
        auto target = temporary.path / "bin/rym-crosscheck.exe";
        std::filesystem::create_directory(target.parent_path());
        std::filesystem::copy_file(cli, target);
        {
            rym::Cache cache(paths.cache, false);
            cache.save_snapshot({{{1,
                                   "Synthetic title",
                                   {"Synthetic artist"},
                                   "",
                                   "https://rateyourmusic.com/song/synthetic/title/"}},
                                 rym::now_seconds()});
        }
        auto args = rym::comparison_arguments(options);
        args.erase(std::find(args.begin(), args.end(), "--clear-cache"));
        auto top = std::find(args.begin(), args.end(), "--top");
        *(top + 1) = "1";
        std::promise<rym::RunOutcome> done;
        auto finished = done.get_future();
        rym::RunSupervisor run(
            target, args, [](auto) {}, [&](auto outcome) { done.set_value(std::move(outcome)); });
        require(finished.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
                "Hidden real CLI completes");
        auto outcome = finished.get();
        require(outcome.exit_code == 0,
                "Hidden real CLI preserves successful exit code: " + outcome.diagnostics);
        require(store.load().report->songs.size() == 1,
                "Hidden real CLI publishes into executable-relative data folder");
    }
}
