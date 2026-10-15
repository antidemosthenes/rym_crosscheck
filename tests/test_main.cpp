#include "rym_crosscheck/cli/application.hpp"
#include "rym_crosscheck/library/scanner.hpp"
#include "rym_crosscheck/matching/index.hpp"
#include "rym_crosscheck/report/markdown.hpp"
#include "rym_crosscheck/rym/chart.hpp"
#include "rym_crosscheck/rym/parser.hpp"
#include "rym_crosscheck/storage/whitelist.hpp"
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <random>
#include <type_traits>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace {
static_assert(std::is_nothrow_move_constructible_v<rym::Track>);
static_assert(std::is_nothrow_move_constructible_v<rym::Song>);
static_assert(std::is_nothrow_move_constructible_v<rym::Identity>);
static_assert(std::is_nothrow_move_constructible_v<rym::Handle>);
static_assert(std::is_nothrow_move_assignable_v<rym::Handle>);
static_assert(std::is_nothrow_move_constructible_v<rym::Index>);
static_assert(std::is_nothrow_move_assignable_v<rym::Index>);
static_assert(std::is_nothrow_move_constructible_v<rym::Browser>);
static_assert(std::is_nothrow_move_assignable_v<rym::Browser>);
static_assert(noexcept(rym::passes({}, {})));
static_assert(noexcept(rym::now_seconds()));
static_assert(!noexcept(rym::normalize("title")));
static_assert(!noexcept(rym::edit_distance(U"a", U"b", 1)));

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}
template <class Function> void throws(Function function) {
    bool failed{};
    try {
        function();
    } catch (const std::exception &) {
        failed = true;
    }
    require(failed, "Expected failure");
}
struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 (L"rym-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(GetTickCount64()));
    Temp() {
        std::filesystem::create_directories(path);
    }
    ~Temp() {
        std::error_code error;
        if (path.filename().wstring().starts_with(L"rym-test-") &&
            path.parent_path() == std::filesystem::temp_directory_path())
            std::filesystem::remove_all(path, error);
    }
};
rym::Track track(std::string title, std::vector<std::string> artists,
                 std::string path = "music.mp3") {
    rym::Track result;
    result.title = std::move(title);
    result.artists = std::move(artists);
    result.path = rym::wide(path);
    return result;
}
rym::Song song(std::string title, std::vector<std::string> artists) {
    return {1, std::move(title), std::move(artists), "",
            "https://rateyourmusic.com/song/example/test/"};
}
void equal_candidates(const auto &left, const auto &right) {
    require(left.size() == right.size(), "Indexed recall differs from exhaustive scorer");
    for (std::size_t i = 0; i < left.size(); ++i) {
        require(left[i].track_id == right[i].track_id && left[i].credit_gap == right[i].credit_gap,
                "Indexed ranking differs");
        require(left[i].artist.numerator == right[i].artist.numerator &&
                    left[i].artist.denominator == right[i].artist.denominator &&
                    left[i].title.numerator == right[i].title.numerator &&
                    left[i].title.denominator == right[i].title.denominator,
                "Scores differ");
    }
}
void normalization() {
    require(rym::normalize("  CAFÉ\t ’Test’—One ").text == "café 'test'-one", "Unicode formatting");
    require(rym::normalize("Cafe\xCC\x81").text == rym::normalize("Café").text,
            "Canonical Unicode");
    require(rym::normalize("Title (2022 Remaster)", true).text == "title", "Remaster suffix");
    require(rym::normalize("Title (Live)", true).text != "title", "Preserve live qualifier");
    require(rym::identity("Live Forever", {"Band"}).version.empty(), "Title word is not version");
    require(rym::threshold("0.900000").numerator == 9, "Rational threshold");
    throws([] { rym::threshold("1.01"); });
    throws([] { rym::normalize(std::string(1, static_cast<char>(0xff))); });
    require(rym::edit_distance(U"kitten", U"sitting", 3) == 3, "Levenshtein reference");
    require(rym::edit_distance(U"kitten", U"sitting", 2) > 2, "Cutoff reference");
    require(rym::passes(rym::similarity(U"abcdefghij", U"abcdxfghij", {9, 10}), {9, 10}),
            "Inclusive boundary");
}
void matching() {
    std::vector<rym::Track> tracks{track("Alpha (2024 Remaster)", {"Band", "Guest"}, "z.mp3"),
                                   track("Alpha", {"Guest", "Band"}, "a.flac"),
                                   track("Alpha (Live)", {"Band", "Guest"}, "live.mp3"),
                                   track("Alphx", {"Band"}, "typo.mp3")};
    rym::Index index(tracks);
    require(index.unique_tracks() == 3, "Duplicate editions grouped");
    require(index.owns(song("Alpha", {"Band", "Guest"})), "Full unordered credits");
    require(!index.owns(song("Alpha", {"Band"})), "Guest credits required");
    auto suggestions = index.suggest(song("Alpha", {"Band"}), {9, 10}, {85, 100});
    require(suggestions.size() == 1 && suggestions[0].credit_gap, "Distinct credit gap review");
    equal_candidates(suggestions, index.suggest(song("Alpha", {"Band"}), {9, 10}, {85, 100}, true));
}
void oracle() {
    std::mt19937 random(4815);
    std::vector<rym::Track> tracks;
    auto name = [&] {
        std::string result;
        auto length = 1 + random() % 18;
        for (std::size_t j = 0; j < length; ++j)
            result += "abcde"[random() % 5];
        return result;
    };
    for (std::size_t i = 0; i < 350; ++i)
        tracks.push_back(track(name(), {name()}, std::to_string(i) + ".mp3"));
    tracks.push_back(track("aaaaabaaaaa", {"Café"}, "unicode.mp3"));
    rym::Index index(tracks);
    for (std::size_t i = 0; i < 450; ++i) {
        const auto &target = tracks[random() % tracks.size()];
        auto query = song(target.title, target.artists);
        if (i % 2 && query.title.size() > 1)
            query.title[random() % query.title.size()] = 'x';
        auto artist = i % 7 == 0   ? rym::Fraction{0, 1}
                      : i % 5 == 0 ? rym::Fraction{1, 1}
                                   : rym::Fraction{9, 10};
        auto title = i % 9 == 0   ? rym::Fraction{0, 1}
                     : i % 5 == 0 ? rym::Fraction{1, 1}
                                  : rym::Fraction{85, 100};
        equal_candidates(index.suggest(query, artist, title),
                         index.suggest(query, artist, title, true));
    }
    auto query = song("aaaaacaaaaa", {"Cafe\xCC\x81"});
    equal_candidates(index.suggest(query, {9, 10}, {85, 100}),
                     index.suggest(query, {9, 10}, {85, 100}, true));
}
void distance_oracle() {
    auto reference = [](std::u32string_view a, std::u32string_view b) {
        std::vector<std::size_t> previous(a.size() + 1), current(a.size() + 1);
        for (std::size_t j = 0; j <= a.size(); ++j)
            previous[j] = j;
        for (std::size_t i = 1; i <= b.size(); ++i) {
            current[0] = i;
            for (std::size_t j = 1; j <= a.size(); ++j)
                current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
                                       previous[j - 1] + (a[j - 1] != b[i - 1])});
            previous.swap(current);
        }
        return previous.back();
    };
    std::vector<std::u32string> strings{U""};
    for (std::size_t length = 1; length <= 5; ++length)
        for (std::size_t mask = 0; mask < (std::size_t{1} << length); ++mask) {
            std::u32string value;
            for (std::size_t bit = 0; bit < length; ++bit)
                value += mask & (std::size_t{1} << bit) ? U'a' : U'b';
            strings.push_back(std::move(value));
        }
    for (const auto &a : strings)
        for (const auto &b : strings) {
            auto expected = reference(a, b);
            for (std::size_t limit = 0; limit <= std::max(a.size(), b.size()); ++limit) {
                auto actual = rym::edit_distance(a, b, limit);
                require(expected <= limit ? actual == expected : actual > limit,
                        "Bounded distance differs from independent DP");
            }
        }
}
void parsing() {
    auto fixture = std::filesystem::path(RYM_FIXTURES);
    auto page = rym::parse_chart(rym::read_file(fixture / "chart.html"),
                                 "https://rateyourmusic.com/charts/top/song/all-time/");
    require(page.songs.size() == 2 && page.songs[0].title == "Alpha & Omega" &&
                page.songs[1].artists.size() == 2,
            "DOM chart metadata");
    require(page.next.ends_with("/2/"), "Actual next link");
    rym::validate_chart(page.songs, 2);
    auto current = rym::parse_chart(rym::read_file(fixture / "chart-current.html"),
                                    "https://rateyourmusic.com/charts/top/song/all-time/");
    require(current.songs[0].rank == 1 && current.songs[0].title == "Starless" &&
                current.songs[0].album == "Red" && current.next.ends_with("/2/"),
            "Current RYM CSS ranks, cover album and pagination");
    require(rym::parse_album(rym::read_file(fixture / "song.html")) == "Original Album",
            "JSON-LD album");
    require(rym::parse_album(rym::read_file(fixture / "song-current.html")) == "Electric Ladyland",
            "Current authoritative song header album");
    throws([&] { rym::validate_chart(page.songs, 3); });
    throws(
        [] { rym::parse_chart("<title>Just a moment...</title>", "https://rateyourmusic.com/"); });
    require(rym::challenged("<title>Security check required</title>"),
            "RYM security gate detected");
    require(!rym::challenged("<title>Security check required by Example Artist</title>"),
            "Song title is not a security gate");
    throws([] { rym::song_url("https://evil.example/song/test/"); });
    throws([] { rym::song_url("https://rateyourmusic.com/artist/test/"); });
}
void acquisition() {
    class Mock final : public rym::Transport {
        std::size_t calls{};

      public:
        std::string fetch(std::string_view url) override {
            ++calls;
            if (url.ends_with("/2/"))
                return "<div class='page_charts_section_charts_item' data-rank='3'><div "
                       "class='page_charts_section_charts_item_title'><a "
                       "href='/song/band/third/'>Third</a></div><div "
                       "class='page_charts_section_charts_item_credited_text'><a class='artist' "
                       "href='/artist/band'>Band</a></div></div>";
            return rym::read_file(std::filesystem::path(RYM_FIXTURES) / "chart.html");
        }
    } mock;
    auto snapshot = rym::fetch_chart(mock, 3);
    require(snapshot.songs.size() == 3 && snapshot.songs[2].rank == 3, "Multi-page acquisition");
    class Broken final : public rym::Transport {
      public:
        std::string fetch(std::string_view) override {
            throw rym::Error("Connection failed");
        }
    } broken;
    throws([&] { rym::fetch_chart(broken, 1); });
}
void album_workers() {
    for (auto [requested, jobs] : std::vector<std::pair<std::size_t, std::size_t>>{
             {1, 11}, {4, 11}, {8, 11}, {8, 3}, {16, 11}}) {
        struct State {
            std::mutex mutex;
            std::condition_variable ready;
            std::vector<std::string> urls;
            std::vector<std::size_t> visits;
            std::size_t width{}, entered{}, active{}, peak{}, factories{};
        } state;
        state.width = std::min({requested, jobs, std::size_t{8}});
        state.visits.resize(jobs);
        rym::Snapshot snapshot;
        std::vector<rym::Missing> missing;
        for (std::size_t i = 0; i < jobs; ++i) {
            auto item = song("Example", {"Band"});
            item.rank = i + 1;
            item.url = "https://rateyourmusic.com/song/band/song-" + std::to_string(i) + "/";
            state.urls.push_back(item.url);
            missing.push_back({item, {}});
            snapshot.songs.push_back(std::move(item));
        }
        class Mock final : public rym::Transport {
            State &state_;

          public:
            explicit Mock(State &state) : state_(state) {}
            std::string fetch(std::string_view url) override {
                std::unique_lock lock(state_.mutex);
                auto found = std::ranges::find(state_.urls, url);
                if (found == state_.urls.end())
                    throw rym::Error("Unexpected album URL");
                ++state_.visits[static_cast<std::size_t>(found - state_.urls.begin())];
                ++state_.active;
                state_.peak = std::max(state_.peak, state_.active);
                ++state_.entered;
                state_.ready.notify_all();
                auto complete = state_.ready.wait_for(
                    lock, std::chrono::seconds(2), [&] { return state_.entered >= state_.width; });
                --state_.active;
                if (!complete)
                    throw rym::Error("Worker rendezvous timed out");
                return "<div class='page_song_header_info_rest'>Track A1 on "
                       "<a class='album' href='/release/album/band/album/'>Album</a></div>";
            }
        };
        rym::enrich_albums(
            missing, snapshot,
            [&]() -> std::unique_ptr<rym::Transport> {
                {
                    std::lock_guard lock(state.mutex);
                    ++state.factories;
                }
                return std::make_unique<Mock>(state);
            },
            requested);
        require(state.factories == state.width && state.peak == state.width && state.active == 0,
                "Album workers respect requested, job-count, and application limits");
        require(std::ranges::all_of(state.visits, [](auto count) { return count == 1; }),
                "Each album job is fetched exactly once");
        require(std::ranges::all_of(snapshot.songs,
                                    [](const auto &item) { return item.album == "Album"; }),
                "Concurrent album results remain attached to their songs");
    }
    rym::Pacer pacer(std::chrono::seconds(10));
    pacer.wait();
    std::stop_source restricted;
    restricted.request_stop();
    throws([&] { pacer.wait(restricted.get_token()); });
}
void tag_priority() {
    Temp temporary;
    std::string body;
    auto frame = [&](std::string_view key, std::string_view value) {
        body += key;
        auto size = value.size() + 1;
        for (int shift : {24, 16, 8, 0})
            body += static_cast<char>((size >> shift) & 255);
        body.append(3, '\0');
        body += value;
    };
    frame("TIT2", "Tagged Title");
    frame("TPE1", "Tagged Artist");
    frame("TALB", "Tagged Album");
    std::string bytes("ID3\3\0\0", 6);
    for (int shift : {21, 14, 7, 0})
        bytes += static_cast<char>((body.size() >> shift) & 127);
    bytes += body;
    std::string audio(417, '\0');
    audio[0] = static_cast<char>(0xff);
    audio[1] = static_cast<char>(0xfb);
    audio[2] = static_cast<char>(0x90);
    audio[3] = static_cast<char>(0x64);
    bytes += audio;
    bytes += audio;
    auto path = temporary.path / "Wrong Artist - Wrong Title.mp3";
    rym::atomic_write(path, bytes);
    auto result = rym::read_track(path);
    require(result.title == "Tagged Title" &&
                result.artists == std::vector<std::string>{"Tagged Artist"} &&
                result.album == "Tagged Album",
            "Tags override filename");
}
void storage_and_scan() {
    Temp temporary;
    auto music = temporary.path / "music";
    std::filesystem::create_directories(music / "nested");
    auto audio = music / "nested" / "Band - Alpha.mp3";
    rym::atomic_write(audio, "placeholder");
    auto cache_path = temporary.path / "cache";
    rym::Cache cache(cache_path);
    auto first = rym::scan_library(music, cache, 3);
    require(first.tracks.size() == 1 && first.tracks[0].title == "Alpha", "Filename fallback");
    cache.save_tracks(music, first.tracks);
    auto second = rym::scan_library(music, cache, 2);
    require(second.reused == 1, "Tag reuse");
    rym::atomic_write(audio, "changed placeholder");
    auto third = rym::scan_library(music, cache, 1);
    require(third.reused == 0, "Size invalidation");
    rym::Snapshot snapshot{{song("Alpha", {"Band"})}, rym::now_seconds()};
    cache.save_snapshot(snapshot);
    require(cache.load_snapshot(1).songs.size() == 1, "Offline snapshot");
    throws([&] { cache.load_snapshot(2); });
    auto list = temporary.path / "whitelist.toml";
    rym::atomic_write(list, "[[songs]]\ntitle = 'ALPHA'\nartist = 'Band'\n");
    rym::Whitelist whitelist(list, true);
    require(whitelist.contains(snapshot.songs[0]), "Whitelist identity");
    rym::atomic_write(list, "[[songs]]\nurl = 'https://evil.example/song/x/'\n");
    throws([&] { rym::Whitelist invalid(list, true); });
    rym::atomic_write(list, "[[songs]]\nurl = 42\ntitle = 'Alpha'\nartist = 'Band'\n");
    throws([&] { rym::Whitelist invalid(list, true); });
    auto report = rym::markdown(snapshot, first, {{snapshot.songs[0], {}}}, music, true);
    require(report.find("[Alpha](<https://rateyourmusic.com/song/example/test/>)") != report.npos &&
                report.find("Offline snapshot") != report.npos,
            "Markdown links/source");
    rym::atomic_write(temporary.path / "report.md", report);
    for (const auto &entry : std::filesystem::recursive_directory_iterator(temporary.path))
        require(entry.path().extension() != ".tmp", "Temporary cleanup");
    throws([&] { rym::Cache competing(cache_path); });
    rym::cancelled.store(true);
    throws([&] { rym::scan_library(music, cache, 2); });
    rym::cancelled.store(false);
    rym::Cache ageing(temporary.path / "ageing");
    ageing.save_snapshot(snapshot);
    std::filesystem::last_write_time(temporary.path / "ageing/chart.json",
                                     std::filesystem::file_time_type::clock::now() -
                                         std::chrono::hours(24 * 31));
    ageing.prune();
    throws([&] { ageing.load_snapshot(1); });
    auto oversized = temporary.path / "ageing/tracks.json";
    rym::atomic_write(oversized, "{}");
    std::filesystem::resize_file(oversized, 128ULL * 1024 * 1024 + 1);
    ageing.prune();
    require(!std::filesystem::exists(oversized), "Cache budget enforced");
    auto directory = temporary.path / "occupied.md";
    std::filesystem::create_directory(directory);
    throws([&] { rym::atomic_write(directory, "cannot replace directory"); });
    for (const auto &entry : std::filesystem::directory_iterator(temporary.path))
        require(entry.path().extension() != ".tmp", "Failed atomic write cleaned");
}
void cli_offline() {
    Temp temporary;
    auto music = temporary.path / "music";
    std::filesystem::create_directories(music);
    rym::atomic_write(music / "Band - Alpha.mp3", "file");
    auto cache_path = temporary.path / "cache";
    {
        rym::Cache cache(cache_path);
        rym::Snapshot snapshot{{song("Alpha", {"Band"})}, rym::now_seconds()};
        cache.save_snapshot(snapshot);
    }
    auto output = temporary.path / "report.md";
    auto args = std::vector<std::string>{"rym-crosscheck",
                                         "--music-dir",
                                         rym::path_text(music),
                                         "--offline",
                                         "--top",
                                         "1",
                                         "--output",
                                         rym::path_text(output),
                                         "--cache-dir",
                                         rym::path_text(cache_path)};
    require(rym::application(args) == 0, "CLI offline result");
    require(rym::read_file(output).find("All compared songs") != std::string::npos,
            "Owned songs excluded");
    for (const auto &workers : {"1", "8"}) {
        auto with_workers = args;
        with_workers.insert(with_workers.end(), {"--web-workers", workers});
        require(rym::application(with_workers) == 0, "Web-worker CLI boundaries accepted");
    }
    auto previous = rym::read_file(output);
    args[5] = "2";
    require(rym::application(args) == 1, "Incomplete chart fails");
    require(rym::read_file(output) == previous, "Previous report preserved");
    rym::cancelled.store(false);
}
void stress() {
    DWORD initial{};
    GetProcessHandleCount(GetCurrentProcess(), &initial);
#ifdef _DEBUG
    _CrtMemState before{}, after{}, difference{};
    _CrtMemCheckpoint(&before);
#endif
    for (int run = 0; run < 30; ++run) {
        Temp temporary;
        rym::Cache cache(temporary.path / "cache");
        std::vector<rym::Track> tracks{track("abcdefghij", {"Artist"})};
        rym::Index index(tracks);
        auto candidates = index.suggest(song("abcdxfghij", {"Artist"}), {9, 10}, {85, 100});
        require(candidates.size() == 1, "Repeated matching");
        auto music = temporary.path / "music";
        std::filesystem::create_directory(music);
        rym::atomic_write(music / "Artist - abcdefghij.mp3", "temporary metadata test");
        auto scan = rym::scan_library(music, cache, 2);
        cache.save_tracks(music, scan.tracks);
        rym::Snapshot snapshot{{song("abcdefghij", {"Artist"})}, rym::now_seconds()};
        cache.save_snapshot(snapshot);
        auto restored = cache.load_snapshot(1);
        rym::atomic_write(temporary.path / "report.md",
                          rym::markdown(restored, scan, {}, music, true));
    }
    DWORD final{};
    GetProcessHandleCount(GetCurrentProcess(), &final);
    require(final <= initial + 2, "Repeated runs leak handles");
#ifdef _DEBUG
    _CrtMemCheckpoint(&after);
    require(_CrtMemDifference(&difference, &before, &after) == 0,
            "Repeated runs retain CRT allocations");
#endif
}
}
void resource_moves() {
    rym::Handle original(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    require(bool(original), "Create move-test event");
    auto native = original.get();
    rym::Handle moved(std::move(original));
    require(!original && moved.get() == native, "Handle move transfers ownership");
    rym::Handle assigned(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    assigned = std::move(moved);
    require(!moved && assigned.get() == native, "Handle move assignment transfers ownership");
    require(SetEvent(assigned.get()) != 0, "Moved handle remains usable");
    std::vector<rym::Track> tracks{track("Alpha", {"Band"})};
    rym::Index index(tracks);
    rym::Index moved_index(std::move(index));
    rym::Index assigned_index(tracks);
    assigned_index = std::move(moved_index);
    require(assigned_index.owns(song("Alpha", {"Band"})), "Moved index retains lookup data");
}
void cli_validation() {
    Temp temporary;
    auto report = temporary.path / "report.md";
    auto cache = temporary.path / "cache";
    auto whitelist = temporary.path / "whitelist.toml";
    rym::atomic_write(report, "previous report");
    rym::atomic_write(whitelist, "");
    for (const auto &option : std::vector<std::vector<std::string>>{{"--web-mode", "browser"},
                                                                    {"--web-mode", "http"},
                                                                    {"--private-browser"},
                                                                    {"--persistent-browser"},
                                                                    {"--web-workers", "0"},
                                                                    {"--web-workers", "9"}}) {
        std::vector<std::string> args{"rym-crosscheck",
                                      "--music-dir",
                                      rym::path_text(temporary.path),
                                      "--output",
                                      rym::path_text(report),
                                      "--cache-dir",
                                      rym::path_text(cache),
                                      "--whitelist",
                                      rym::path_text(whitelist),
                                      "--offline",
                                      "--top",
                                      "1"};
        args.insert(args.end(), option.begin(), option.end());
        require(rym::application(args) == 1,
                "Unsupported options and worker counts must fail parsing");
        require(!std::filesystem::exists(cache),
                "Invalid CLI must not initialize cache or retrieval");
        require(rym::read_file(report) == "previous report", "Invalid CLI must preserve report");
    }
}
int main() {
    rym::install_cancellation();
    int failed{};
    for (const auto &[name, function] : std::vector<std::pair<std::string, std::function<void()>>>{
             {"normalization", normalization},
             {"matching", matching},
             {"indexed oracle", oracle},
             {"distance oracle", distance_oracle},
             {"HTML parsing", parsing},
             {"chart acquisition", acquisition},
             {"album workers", album_workers},
             {"tag priority", tag_priority},
             {"storage/scanning", storage_and_scan},
             {"CLI offline", cli_offline},
             {"resource moves", resource_moves},
             {"CLI validation", cli_validation},
             {"lifecycle stress", stress}}) {
        try {
            function();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            ++failed;
            rym::cancelled.store(false);
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failed ? 1 : 0;
}
