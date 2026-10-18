#include "rym_crosscheck/cli/application.hpp"
#include "rym_crosscheck/cli/launcher.hpp"
#include "rym_crosscheck/library/scanner.hpp"
#include "rym_crosscheck/matching/index.hpp"
#include "rym_crosscheck/report/report.hpp"
#include "rym_crosscheck/rym/chart.hpp"
#include "rym_crosscheck/storage/whitelist.hpp"
#include <CLI/CLI.hpp>
#include <future>
#include <iostream>
#include <shellapi.h>

namespace rym {
int application(const std::vector<std::string> &arguments,
                const std::filesystem::path &application_directory) {
    install_cancellation();
    auto paths = PortablePaths::from_directory(
        application_directory.empty() ? cli_installation_directory() : application_directory);
    auto options = default_options(paths);
    auto &music = options.music, &cache_dir = options.cache, &profile = options.profile;
    auto &artist_value = options.artist, &title_value = options.title;
    auto &top = options.top, &local_workers = options.local_workers,
         &web_workers = options.web_workers;
    auto &delay = options.delay, &verification = options.verification;
    auto &offline = options.offline, &clear = options.clear, &no_open = options.no_open;
    std::uint64_t event_handle{}, cancel_handle{};
    std::string launcher_id;
    std::unique_ptr<LauncherChannel> channel;
    CLI::App app{"Find missing songs from RYM's all-time song chart", "rym-crosscheck"};
    app.add_option("--music-dir", music, "Required local music folder")->required();
    app.add_option("--top", top, "Number of chart songs")
        ->check(CLI::PositiveNumber)
        ->check(CLI::Range(1, INT_MAX))
        ->default_val(200);
    app.add_option("--cache-dir", cache_dir, "Application cache")->default_val(cache_dir);
    app.add_option("--browser-profile", profile, "Dedicated Edge profile");
    app.add_option("--local-workers", local_workers, "Tag-reading workers (1..64)")
        ->check(CLI::Range(1, 64));
    app.add_option("--web-workers", web_workers, "Album lookup workers (1..8)")
        ->check(CLI::Range(1, static_cast<int>(max_web_workers)))
        ->default_val(web_workers);
    app.add_option("--request-delay-ms", delay, "Global request start interval")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--verification-timeout", verification,
                   "Seconds to complete security verification or manual sign-in")
        ->check(CLI::Range(1, 3600));
    app.add_option("--artist-threshold", artist_value, "Artist similarity between 0 and 1");
    app.add_option("--title-threshold", title_value, "Title similarity between 0 and 1");
    app.add_flag("--offline", offline, "Use a snapshot created by a previous live run");
    app.add_flag("--clear-cache", clear, "Clear application cache before comparison");
    app.add_flag("--no-open", no_open, "Do not open the report reviewer after comparison");
    app.add_option("--launcher-events", event_handle)->group("");
    app.add_option("--launcher-cancel", cancel_handle)->group("");
    app.add_option("--launcher-id", launcher_id)->group("");
    std::vector<std::string> reversed(arguments.rbegin(), arguments.rend());
    if (!reversed.empty())
        reversed.pop_back();
    try {
        app.parse(reversed);
    } catch (const CLI::ParseError &error) {
        app.exit(error);
        return error.get_exit_code() == 0 ? 0 : 1;
    }
    bool published{};
    int completed_result{};
    try {
        if (event_handle || cancel_handle || !launcher_id.empty())
            channel = std::make_unique<LauncherChannel>(event_handle, cancel_handle, launcher_id);
        options = validate_options(std::move(options));
        auto artist_cutoff = threshold(artist_value), title_cutoff = threshold(title_value);
        auto root =
            std::filesystem::absolute(std::filesystem::path(wide(music))).lexically_normal();
        if (!std::filesystem::is_directory(root))
            throw Error("Music folder does not exist: " + music);
        ReportStore store(paths);
        RunLock run(paths);
        store.initialize();
        Whitelist ignored;
        {
            ReportLock lock(store.paths());
            ignored = Whitelist(store.paths().whitelist, true);
        }
        Cache cache(wide(cache_dir), clear);
        auto started = std::chrono::steady_clock::now();
        if (channel)
            channel->emit("acquiring", {{"message", "Scanning local files and loading the chart"},
                                        {"top", top},
                                        {"offline", offline}});
        auto local = std::async(std::launch::async, [&] {
            auto scanned = scan_library(root, cache, local_workers);
            if (channel)
                channel->emit({}, {{"local_files", scanned.tracks.size()},
                                   {"cached", scanned.reused},
                                   {"unresolved", scanned.warnings.size()}});
            return scanned;
        });
        struct CancelOnFailure {
            ~CancelOnFailure() noexcept {
                if (std::uncaught_exceptions())
                    cancelled.store(true);
            }
        } cancel_on_failure;
        Snapshot snapshot;
        Scan scan;
        Pacer pacer{std::chrono::milliseconds(delay)};
        std::unique_ptr<Browser> transport;
        auto chart_progress = [&](std::size_t count) {
            if (channel)
                channel->emit("acquiring", {{"message", offline ? "Loaded saved chart snapshot"
                                                                : "Retrieving RYM chart songs"},
                                            {"chart_songs", count},
                                            {"top", top}});
        };
        if (offline) {
            snapshot = cache.load_snapshot(top);
            chart_progress(snapshot.songs.size());
        } else {
            auto selected_profile =
                profile.empty() ? default_profile() : std::filesystem::path(wide(profile));
            transport = std::make_unique<Browser>(selected_profile, pacer, channel != nullptr);
            AccessTransport chart_transport(
                [&](std::string_view url) { return transport->fetch(url); },
                [&](std::string_view url, AccessReason reason) {
                    transport.reset();
                    if (channel)
                        channel->emit(
                            reason == AccessReason::sign_in ? "sign_in" : "verification",
                            {{"message", reason == AccessReason::sign_in
                                             ? "Sign in to RYM in Edge. Once the chart or your "
                                               "signed-in homepage appears, close that Edge "
                                               "window to continue."
                                             : "Complete any RYM security check in Edge. If shown, "
                                               "click Submit after reCAPTCHA. Wait for the chart, "
                                               "then close Edge to continue. If only the normal "
                                               "homepage appears without an access prompt, close "
                                               "Edge to retry chart retrieval."}});
                    resolve_browser_access(selected_profile, std::chrono::seconds(verification),
                                           channel != nullptr, url, reason);
                    if (channel)
                        channel->emit("acquiring",
                                      {{"message", "Edge window closed; retrieving chart"}});
                    transport =
                        std::make_unique<Browser>(selected_profile, pacer, channel != nullptr);
                });
            snapshot = fetch_chart(chart_transport, top, chart_progress);
        }
        scan = local.get();
        check_cancelled();
        if (channel)
            channel->emit("matching", {{"message", "Matching chart songs against local files"},
                                       {"chart_songs", snapshot.songs.size()},
                                       {"top", top},
                                       {"local_files", scan.tracks.size()},
                                       {"unresolved", scan.warnings.size()},
                                       {"cached", scan.reused}});
        auto report = compare_library(snapshot, scan, root, offline, artist_cutoff, title_cutoff);
        std::vector<Missing> missing;
        for (const auto &row : report.songs)
            if (!row.owned && !ignored.contains(row.song))
                missing.push_back({row.song, {}});
        if (!offline) {
            if (channel)
                channel->emit("albums", {{"message", "Looking up associated albums"},
                                         {"results", missing.size()}});
            auto factory = [&] { return transport->fork(); };
            enrich_albums(missing, snapshot, factory, web_workers);
            for (auto &row : report.songs)
                row.song.album = snapshot.songs[row.song.rank - 1].album;
        }
        transport.reset();
        check_cancelled();
        try {
            cache.save_tracks(root, scan.tracks);
            if (!offline)
                cache.save_snapshot(snapshot);
            cache.prune();
        } catch (const std::exception &error) {
            log("Cache warning: " + std::string(error.what()));
        }
        check_cancelled();
        if (channel)
            channel->emit("publishing", {{"message", "Writing the completed report"}});
        report = store.publish(std::move(report));
        published = true;
        completed_result = scan.warnings.empty() ? 0 : 2;
        if (channel)
            channel->emit("complete",
                          {{"message", "Comparison completed"}, {"exit_code", completed_result}});
        Whitelist final_whitelist(report.whitelist);
        auto attention = std::ranges::count_if(report.songs, [&](const auto &row) {
            return !row.owned && !final_whitelist.contains(row.song);
        });
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
        std::cout << "Compared " << snapshot.songs.size() << " songs with " << scan.tracks.size()
                  << " files (" << scan.reused << " cached). " << attention
                  << " missing/review rows.\nReport: " << path_text(store.paths().report)
                  << "\nElapsed: " << milliseconds << " ms\n";
        DWORD mode{};
        if (!no_open && interactive() && GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) {
            auto viewer = store.paths().viewer;
            auto launched = reinterpret_cast<INT_PTR>(
                ShellExecuteW(nullptr, L"open", viewer.c_str(), L"--show-results",
                              viewer.parent_path().c_str(), SW_SHOWMAXIMIZED));
            if (launched <= 32)
                log("Report saved, but the reviewer could not be opened. Run "
                    "missing-report.exe manually.");
        }
        return completed_result;
    } catch (const std::exception &error) {
        if (published) {
            log("Report saved; post-comparison warning: " + std::string(error.what()));
            return completed_result;
        }
        if (channel)
            channel->emit("error", {{"message", error.what()}, {"cancelled", cancelled.load()}});
        cancelled.store(true);
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
}
