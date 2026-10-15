#include "rym_crosscheck/cli/application.hpp"
#include "rym_crosscheck/library/scanner.hpp"
#include "rym_crosscheck/matching/index.hpp"
#include "rym_crosscheck/report/markdown.hpp"
#include "rym_crosscheck/rym/chart.hpp"
#include "rym_crosscheck/storage/whitelist.hpp"
#include <CLI/CLI.hpp>
#include <future>
#include <iostream>

namespace rym {
int application(const std::vector<std::string> &arguments) {
    install_cancellation();
    std::string music, output = "missing.md", whitelist = "whitelist.toml",
                       cache_dir = ".rym-crosscheck-cache", profile;
    std::string artist_value = "0.90", title_value = "0.85";
    std::size_t top = 200,
                local_workers = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8),
                web_workers = default_web_workers;
    int delay = 1000, verification = 300;
    bool offline{}, clear{};
    CLI::App app{"Find missing songs from RYM's all-time song chart", "rym-crosscheck"};
    app.add_option("--music-dir", music, "Required local music folder")->required();
    app.add_option("--top", top, "Number of chart songs")
        ->check(CLI::PositiveNumber)
        ->default_val(200);
    app.add_option("--output", output, "Markdown output")->default_val(output);
    auto *whitelist_option =
        app.add_option("--whitelist", whitelist, "TOML whitelist")->default_val(whitelist);
    app.add_option("--cache-dir", cache_dir, "Application cache")->default_val(cache_dir);
    app.add_option("--browser-profile", profile, "Dedicated Edge profile");
    app.add_option("--local-workers", local_workers, "Tag-reading workers (1..64)")
        ->check(CLI::Range(1, 64));
    app.add_option("--web-workers", web_workers, "Album lookup workers (1..8)")
        ->check(CLI::Range(1, static_cast<int>(max_web_workers)))
        ->default_val(web_workers);
    app.add_option("--request-delay-ms", delay, "Global request start interval")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--verification-timeout", verification, "Seconds to complete verification")
        ->check(CLI::Range(1, 3600));
    app.add_option("--artist-threshold", artist_value, "Artist similarity between 0 and 1");
    app.add_option("--title-threshold", title_value, "Title similarity between 0 and 1");
    app.add_flag("--offline", offline, "Use a snapshot created by a previous live run");
    app.add_flag("--clear-cache", clear, "Clear application cache before comparison");
    std::vector<std::string> reversed(arguments.rbegin(), arguments.rend());
    if (!reversed.empty())
        reversed.pop_back();
    try {
        app.parse(reversed);
    } catch (const CLI::ParseError &error) {
        app.exit(error);
        return error.get_exit_code() == 0 ? 0 : 1;
    }
    try {
        auto artist_cutoff = threshold(artist_value), title_cutoff = threshold(title_value);
        auto root =
            std::filesystem::absolute(std::filesystem::path(wide(music))).lexically_normal();
        if (!std::filesystem::is_directory(root))
            throw Error("Music folder does not exist: " + music);
        auto report_path = std::filesystem::absolute(wide(output)).lexically_normal();
        if (normalize(path_text(report_path.extension())).text != ".md")
            throw Error("Output must have a .md extension.");
        auto whitelist_path = std::filesystem::absolute(wide(whitelist)).lexically_normal();
        if (report_path == whitelist_path)
            throw Error("Output cannot overwrite the whitelist.");
        Whitelist ignored(whitelist_path, whitelist_option->count() != 0);
        Cache cache(wide(cache_dir), clear);
        auto started = std::chrono::steady_clock::now();
        auto local = std::async(std::launch::async,
                                [&] { return scan_library(root, cache, local_workers); });
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
        if (offline)
            snapshot = cache.load_snapshot(top);
        else {
            auto selected_profile =
                profile.empty() ? default_profile() : std::filesystem::path(wide(profile));
            transport = std::make_unique<Browser>(selected_profile, pacer);
            try {
                snapshot = fetch_chart(*transport, top);
            } catch (const VerificationRequired &) {
                transport.reset();
                verify_browser(selected_profile, std::chrono::seconds(verification));
                transport = std::make_unique<Browser>(selected_profile, pacer);
                snapshot = fetch_chart(*transport, top);
            }
        }
        scan = local.get();
        check_cancelled();
        Index index(scan.tracks);
        std::vector<Missing> missing;
        for (const auto &song : snapshot.songs) {
            check_cancelled();
            auto normalized = identity(song.title, song.artists);
            if (ignored.contains(song, normalized) || index.owns(normalized))
                continue;
            missing.push_back({song, index.suggest(normalized, artist_cutoff, title_cutoff)});
        }
        if (!offline) {
            auto factory = [&] { return transport->fork(); };
            enrich_albums(missing, snapshot, factory, web_workers);
            for (auto &row : missing)
                row.song.album = snapshot.songs[row.song.rank - 1].album;
        }
        transport.reset();
        check_cancelled();
        atomic_write(report_path, markdown(snapshot, scan, missing, root, offline));
        try {
            cache.save_tracks(root, scan.tracks);
            if (!offline)
                cache.save_snapshot(snapshot);
        } catch (const std::exception &error) {
            log("Cache warning: " + std::string(error.what()));
        }
        cache.prune();
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
        std::cout << "Compared " << snapshot.songs.size() << " songs with " << scan.tracks.size()
                  << " files (" << scan.reused << " cached). " << missing.size()
                  << " missing/review rows.\nReport: " << path_text(report_path)
                  << "\nElapsed: " << milliseconds << " ms\n";
        return scan.warnings.empty() ? 0 : 2;
    } catch (const std::exception &error) {
        cancelled.store(true);
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
}
