#include "rym_crosscheck/report/report.hpp"
#include <functional>
#include <thread>

namespace {
void require(bool value, std::string_view message) {
    if (!value)
        throw rym::Error(std::string(message));
}
void rejects(const std::function<void()> &action) {
    bool failed{};
    try {
        action();
    } catch (const std::exception &) {
        failed = true;
    }
    require(failed, "Expected report-store rejection");
}
struct Temporary {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 (L"rym-review-test-" + std::to_wstring(GetCurrentProcessId()) +
                                  L"-" + std::to_wstring(GetTickCount64()));
    Temporary() {
        std::filesystem::create_directories(path);
    }
    ~Temporary() {
        std::error_code error;
        if (path.parent_path() == std::filesystem::temp_directory_path() &&
            path.filename().native().starts_with(L"rym-review-test-"))
            std::filesystem::remove_all(path, error);
    }
};
rym::Song make_song(std::size_t rank, std::string title, std::string slug) {
    return {rank,
            std::move(title),
            {"Artist", "Guest & Co."},
            "",
            "https://rateyourmusic.com/song/example/" + slug + "/"};
}
}
void report_review_tests() {
    Temporary temporary;
    auto paths = rym::PortablePaths::from_directory(temporary.path);
    rym::atomic_write(temporary.path / "whitelist.toml",
                      "# legacy comment\n[[songs]]\nartist='Artist'\ntitle='Older song'\n");
    rym::ReportStore store(paths);
    store.initialize();
    require(std::filesystem::exists(temporary.path / "whitelist.toml") &&
                std::filesystem::exists(paths.whitelist),
            "Legacy whitelist remains untouched");
    require(rym::Whitelist(paths.whitelist, true).entries().empty(),
            "No implicit whitelist migration");
    rym::atomic_write(temporary.path / "whitelist.toml", "broken legacy [");
    store.initialize();
    require(std::filesystem::exists(temporary.path / "whitelist.toml"),
            "Existing new whitelist takes precedence");
    auto initial = store.load();
    require(!initial.report && initial.whitelist.entries().empty(),
            "Initial state has no fake report");
    store.replace(rym::Whitelist::parse("[[songs]]\nartist='Artist'\ntitle='Older song'\n"));
    auto music = temporary.path / "music";
    std::filesystem::create_directory(music);
    rym::Snapshot snapshot{{make_song(1, "Café </script> & 'song'", "one"),
                            make_song(2, "Second song", "two"),
                            make_song(3, "Owned song", "three")},
                           rym::now_seconds()};
    rym::Scan scan;
    scan.tracks.push_back(
        {music / "owned.flac", "Owned song", {"Guest & Co.", "Artist"}, "Other album", 1, 1});
    scan.tracks.push_back(
        {music / "near.flac", "Second songs", {"Artist", "Guest & Co."}, "", 1, 1});
    scan.warnings = {"Unresolved: unknown.mp3"};
    scan.reused = 1;
    auto report = rym::compare_library(snapshot, scan, music, true, {9, 10}, {85, 100});
    require(report.songs[2].owned && !report.songs[1].candidates.empty(),
            "Ownership and candidate data retained");
    auto restored = rym::parse_report(rym::report_json(report));
    require(restored.songs[0].song.title == snapshot.songs[0].title &&
                restored.warnings == scan.warnings && restored.offline && restored.reused == 1,
            "JSON preserves Unicode, hostile text and scan context");
    require(restored.songs[1].candidates[0].path == "near.flac", "Candidate paths remain relative");
    for (const auto *field : {"generated", "captured"}) {
        auto future = rym::report_json(report);
        future[field] = 253402300800LL;
        rejects([&] { rym::parse_report(future); });
        future[field] = 253402300799LL;
        rym::parse_report(future);
    }
    auto invalid = rym::report_json(report);
    invalid["schema_version"] = 99;
    rejects([&] { rym::parse_report(invalid); });
    invalid = rym::report_json(report);
    invalid["songs"].as_array()[1].as_object()["candidates"].as_array()[0].as_object()["path"] =
        "../outside.flac";
    rejects([&] { rym::parse_report(invalid); });
    invalid = rym::report_json(report);
    invalid["songs"]
        .as_array()[1]
        .as_object()["candidates"]
        .as_array()[0]
        .as_object()["artist"]
        .as_object()["denominator"] = 0;
    rejects([&] { rym::parse_report(invalid); });
    report = store.publish(std::move(report));
    auto baseline = store.load();
    require(baseline.revision == store.revision(), "Loaded store revision matches current files");
    const auto first_url = snapshot.songs[0].url, second_url = snapshot.songs[1].url;
    auto saved = store.apply(baseline, {first_url, first_url}, {});
    require(saved.document.revision == store.revision(),
            "Saved revision prevents reloading the reviewer's own writes");
    require(saved.warning.empty() && saved.document.whitelist.entries().size() == 2,
            "Selected additions deduplicate");
    auto view = rym::review_json(saved.document);
    require(view.at("songs").as_array()[0].as_object().at("whitelisted").as_bool(),
            "Addition immediately leaves current results");
    const auto key = "url:" + first_url;
    auto newer = report;
    newer.generation = "newer-generation";
    store.publish(newer);
    auto external = saved.document.whitelist.entries();
    external.push_back({{}, second_url, {}, {}});
    rym::atomic_write(paths.whitelist, rym::Whitelist(external).toml());
    require(store.differs(saved.document.whitelist),
            "External entries require a departure decision even without selections");
    auto removed = store.apply(saved.document, {}, {key});
    require(removed.document.report->generation == "newer-generation",
            "Old reviewer preserves newer active report");
    require(!removed.document.whitelist.contains(snapshot.songs[1]) &&
                !removed.document.whitelist.contains(snapshot.songs[0]),
            "Authoritative replacement removes external entries absent from the view");
    view = rym::review_json(removed.document);
    require(!view.at("songs").as_array()[0].as_object().at("whitelisted").as_bool(),
            "Removed entry immediately returns");
    auto previous = rym::read_file(paths.whitelist);
    rejects(
        [&] { store.apply(removed.document, {"https://rateyourmusic.com/song/unknown/no/"}, {}); });
    rejects([&] { store.apply(removed.document, {}, {"unknown-key"}); });
    require(rym::read_file(paths.whitelist) == previous, "Invalid selections preserve whitelist");
    rym::atomic_write(paths.whitelist, "invalid = [");
    rejects([&] { store.differs(removed.document.whitelist); });
    rejects([&] { store.apply(removed.document, {first_url}, {}); });
    require(rym::read_file(paths.whitelist) == "invalid = [",
            "Malformed whitelist is not overwritten");
    rym::atomic_write(paths.whitelist, previous);
    require(SetFileAttributesW(paths.whitelist.c_str(), FILE_ATTRIBUTE_READONLY),
            "Make read-only whitelist fixture");
    rejects([&] { store.apply(removed.document, {first_url}, {}); });
    require(rym::read_file(paths.whitelist) == previous, "Permission failure preserves whitelist");
    SetFileAttributesW(paths.whitelist.c_str(), FILE_ATTRIBUTE_NORMAL);
    require(SetFileAttributesW(paths.report.c_str(), FILE_ATTRIBUTE_READONLY),
            "Make read-only report fixture");
    auto partial = store.apply(removed.document, {first_url}, {});
    require(!partial.warning.empty() &&
                rym::Whitelist(paths.whitelist, true).contains(snapshot.songs[0]),
            "Whitelist save survives report refresh failure");
    SetFileAttributesW(paths.report.c_str(), FILE_ATTRIBUTE_NORMAL);
    store.publish(newer);
    auto concurrent = store.load();
    auto extra_url = "https://rateyourmusic.com/song/example/off-chart/";
    auto entries = concurrent.whitelist.entries();
    entries.push_back({{}, extra_url, {}, {}});
    rym::atomic_write(paths.whitelist, rym::Whitelist(entries).toml());
    auto proposed = rym::proposed_whitelist(concurrent, {}, {key});
    require(store.differs(proposed), "Proposed removal differs from external disk edits");
    require(rym::read_file(paths.whitelist) == rym::Whitelist(entries).toml(),
            "Checking or cancelling a decision leaves disk unchanged");
    store.replace(proposed);
    auto merged = store.load();
    require(!merged.whitelist.contains(snapshot.songs[0]) &&
                !merged.whitelist.contains(snapshot.songs[1]),
            "Displayed removal is applied");
    require(!std::ranges::any_of(merged.whitelist.entries(),
                                 [&](const auto &entry) { return entry.url == extra_url; }),
            "Replace writes only the displayed set");
    auto canonical =
        rym::Whitelist::parse("songs = [{title='Title',artists=['Artist','Guest & "
                              "Co.']},{url='https://rateyourmusic.com/song/example/one/'}]");
    require(rym::Whitelist::parse(canonical.toml()).entries().size() == 2,
            "Inline TOML converts without losing entries");
    store.replace(rym::Whitelist(
        {{{}, first_url, {}, {}}, {{}, second_url, {}, {}}, {{}, extra_url, {}, {}}}));
    auto shared = store.load();
    std::exception_ptr failure1, failure2;
    std::jthread writer1([&] {
        try {
            store.apply(shared, {}, {key});
        } catch (...) {
            failure1 = std::current_exception();
        }
    });
    std::jthread writer2([&] {
        try {
            store.apply(shared, {}, {"url:" + second_url});
        } catch (...) {
            failure2 = std::current_exception();
        }
    });
    writer1.join();
    writer2.join();
    if (failure1)
        std::rethrow_exception(failure1);
    if (failure2)
        std::rethrow_exception(failure2);
    auto final = store.load();
    require(final.whitelist.entries().size() == 2 && (final.whitelist.contains(snapshot.songs[0]) !=
                                                      final.whitelist.contains(snapshot.songs[1])),
            "Concurrent saves serialize complete proposed sets rather than merging them");
    auto input = temporary.path / "source-report.json";
    auto input_whitelist = temporary.path / "source-whitelist.toml";
    auto imported_report = newer;
    imported_report.whitelist = canonical.entries();
    rym::write_json(input, rym::report_json(imported_report));
    auto source_before = rym::read_file(input), active_before = rym::read_file(paths.report);
    auto preview = rym::inspect_import(paths, input, true);
    require(preview.count() == 3 && rym::read_file(paths.report) == active_before,
            "Import preview and cancellation preserve destinations");
    auto whitelist_before = rym::read_file(paths.whitelist);
    auto imported = store.import_file(preview);
    require(rym::read_file(input) == source_before &&
                rym::read_file(paths.whitelist) == whitelist_before,
            "JSON import copies and never adopts the embedded whitelist");
    require(imported.document.report->generation != imported_report.generation &&
                imported.document.report->whitelist.size() ==
                    imported.document.whitelist.entries().size(),
            "Imported JSON gets a local generation and active whitelist snapshot");
    require(!std::filesystem::exists(paths.cache), "Report import never seeds offline caches");
    rym::atomic_write(input_whitelist, canonical.toml());
    auto whitelist_source = rym::read_file(input_whitelist);
    auto whitelist_preview = rym::inspect_import(paths, input_whitelist, false);
    auto loaded = store.import_file(whitelist_preview);
    require(loaded.document.whitelist.entries().size() == 2 &&
                rym::read_file(input_whitelist) == whitelist_source,
            "Whitelist import preserves its source and refreshes both sections");
    require(store.import_file(rym::inspect_import(paths, paths.whitelist, false))
                    .document.report->generation == loaded.document.report->generation,
            "Selecting active destination is a no-op");
    rym::atomic_write(input_whitelist, "songs=[ invalid");
    auto retained = rym::read_file(paths.whitelist);
    rejects([&] { rym::inspect_import(paths, input_whitelist, false); });
    require(rym::read_file(paths.whitelist) == retained,
            "Invalid imports preserve active contents");
    require(SetFileAttributesW(paths.report.c_str(), FILE_ATTRIBUTE_READONLY),
            "Read-only import destination");
    auto report_before = rym::read_file(paths.report);
    rejects([&] { store.import_file(preview); });
    require(rym::read_file(paths.report) == report_before && rym::read_file(input) == source_before,
            "Failed import preserves destination and source");
    SetFileAttributesW(paths.report.c_str(), FILE_ATTRIBUTE_NORMAL);
    rym::atomic_write(paths.report, "broken JSON");
    auto available = store.available();
    require(!available.document.report && available.document.whitelist.entries().size() == 2 &&
                !available.warning.empty(),
            "Malformed report leaves whitelist independently available");
    rym::atomic_write(paths.whitelist, "broken TOML [");
    auto recovered = store.import_file(preview);
    require(recovered.document.report && !recovered.document.whitelist_valid &&
                !recovered.warning.empty() && rym::read_file(paths.whitelist) == "broken TOML [",
            "Report import is independent of malformed active TOML");
    rejects([&] { rym::proposed_whitelist(recovered.document, {}, {}); });
    rejects([&] { store.apply(recovered.document, {}, {}); });
    require(rym::read_file(paths.whitelist) == "broken TOML [",
            "An unreadable displayed whitelist cannot become an empty replacement");
    auto broken_paths = rym::PortablePaths::from_directory(temporary.path / "broken");
    rym::atomic_write(broken_paths.application / "whitelist.toml", "not valid [");
    rym::ReportStore(broken_paths).initialize();
    require(std::filesystem::exists(broken_paths.application / "whitelist.toml") &&
                std::filesystem::exists(broken_paths.whitelist),
            "Invalid legacy file stays in place and is never adopted");
    for (const auto &item : std::filesystem::recursive_directory_iterator(temporary.path))
        require(item.path().extension() != ".tmp", "Report operations clean temporary files");
}
