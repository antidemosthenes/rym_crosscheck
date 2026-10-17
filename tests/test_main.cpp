#include "rym_crosscheck/cli/application.hpp"
#include "rym_crosscheck/library/scanner.hpp"
#include "rym_crosscheck/matching/index.hpp"
#include "rym_crosscheck/report/report.hpp"
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
    require(rym::challenged(
                "<p>some requests from your computer might be coming from automated "
                "scripts</p><form><div class='g-recaptcha'></div><button>Submit</button></form>"),
            "RYM reCAPTCHA restriction form is recognized without Cloudflare markers");
    require(!rym::challenged("<form>Sign in<div class='g-recaptcha'></div></form>"),
            "A sign-in form with reCAPTCHA is not mistaken for a security gate");
    using State = rym::BrowserPageState;
    constexpr auto chart = "https://rateyourmusic.com/charts/top/song/all-time/";
    constexpr auto second = "https://rateyourmusic.com/charts/top/song/all-time/2/";
    require(rym::browser_page_state("https://rateyourmusic.com/account/login?redirect=chart",
                                    second, "complete", "<form>Sign in</form>") == State::sign_in,
            "RYM sign-in redirect is distinct from a Cloudflare challenge");
    require(rym::browser_page_state(
                "https://rateyourmusic.com/account/login", second, "complete",
                "<p>loading pages too quickly</p><div class='g-recaptcha'></div>") ==
                State::verification,
            "A real restriction form takes precedence over its URL");
    require(rym::browser_page_state(second, second, "complete",
                                    "<title>Unrecognized gate</title>") == State::loading &&
                rym::browser_page_state(second, second, "complete",
                                        "<title>Unrecognized gate</title>",
                                        true) == State::verification,
            "Unrecognized unusable chart requests verification after its deadline");
    require(rym::browser_page_state(chart, second, "complete", "page_charts_section_charts_item") ==
                State::loading,
            "Previous chart document cannot satisfy the next page request");
    require(rym::browser_page_state("https://rateyourmusic.com/charts/top/song/all-time/2/#top",
                                    second, "complete",
                                    "page_charts_section_charts_item") == State::ready,
            "Usable chart accepts a fragment and canonical trailing slash");
    require(rym::browser_page_state(second, second, "loading", "<title>Just a moment...</title>") ==
                State::verification,
            "Known challenge requests verification before document completion");
    throws([] { rym::song_url("https://evil.example/song/test/"); });
    throws([] { rym::song_url("https://rateyourmusic.com/artist/test/"); });
}
void browser_protocol() {
    auto response = rym::browser_response(
        R"({"id":7,"result":{"value":{"title":"Café 日本語","items":["a","b"]}}})", 7);
    require(response.has_value() && !response->storage().is_not_shared_and_deallocate_is_trivial(),
            "Matching browser response does not borrow a temporary parse arena");
    std::vector<std::string> pressure(32, std::string(8192, 'x'));
    require(response->at("value").as_object().at("title").as_string() == "Café 日本語" &&
                response->at("value").as_object().at("items").as_array().size() == 2,
            "Returned browser JSON owns its parse resource after the parse scope ends");
    auto moved = std::move(*response);
    response.reset();
    require(moved.at("value").as_object().at("title").as_string() == "Café 日本語",
            "Browser response storage survives moves");
    require(!rym::browser_response(R"({"method":"Page.loadEventFired","params":{}})", 7) &&
                !rym::browser_response(R"({"id":8,"error":{"message":"Other command"}})", 7),
            "Asynchronous browser events and other replies do not satisfy a command");
    throws([] { rym::browser_response(R"({"id":7,"error":{"message":"Failed"}})", 7); });
    throws([] { rym::browser_response("broken JSON", 7); });
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
    std::vector<std::string> requested, verified;
    std::vector<std::size_t> collected;
    std::size_t second_calls{};
    rym::AccessTransport recovering(
        [&](std::string_view url) {
            requested.emplace_back(url);
            if (url.ends_with("/2/") && ++second_calls == 1)
                throw rym::VerificationRequired("Unrecognized gate reached its deadline");
            return mock.fetch(url);
        },
        [&](std::string_view url, rym::AccessReason) { verified.emplace_back(url); });
    auto recovered =
        rym::fetch_chart(recovering, 3, [&](auto count) { collected.push_back(count); });
    require(
        recovered.songs.size() == 3 && requested.size() == 3 && second_calls == 2 &&
            verified ==
                std::vector<std::string>{"https://rateyourmusic.com/charts/top/song/all-time/2/"} &&
            collected == std::vector<std::size_t>{2, 3},
        "Later-page verification retries that URL without restarting collected ranks");
    std::size_t attempts{}, prompts{};
    rym::AccessTransport blocked(
        [&](std::string_view) -> std::string {
            ++attempts;
            throw rym::VerificationRequired("Still blocked");
        },
        [&](std::string_view, rym::AccessReason) { ++prompts; });
    throws([&] { rym::fetch_chart(blocked, 1); });
    require(attempts == 2 && prompts == 1, "Persistent blocking cannot create a verification loop");
    class Broken final : public rym::Transport {
      public:
        std::string fetch(std::string_view) override {
            throw rym::Error("Connection failed");
        }
    } broken;
    throws([&] { rym::fetch_chart(broken, 1); });
    rym::AccessTransport disconnected([&](auto url) { return broken.fetch(url); },
                                      [&](std::string_view, rym::AccessReason) { ++prompts; });
    throws([&] { rym::fetch_chart(disconnected, 1); });
    require(prompts == 1, "Connection errors do not open a verification window");
    std::vector<rym::AccessReason> reasons;
    std::size_t access_attempts{};
    rym::AccessTransport access(
        [&](std::string_view) {
            if (++access_attempts == 1)
                throw rym::VerificationRequired("Security check");
            if (access_attempts == 2)
                throw rym::SignInRequired("RYM requests sign-in");
            return std::string("chart");
        },
        [&](std::string_view, rym::AccessReason reason) { reasons.push_back(reason); });
    require(access.fetch("https://rateyourmusic.com/charts/top/song/all-time/2/") == "chart" &&
                reasons == std::vector<rym::AccessReason>{rym::AccessReason::verification,
                                                          rym::AccessReason::sign_in},
            "Security verification and manual sign-in are distinct bounded recovery steps");
    std::size_t sign_in_attempts{}, sign_in_prompts{};
    rym::AccessTransport signed_out(
        [&](std::string_view) -> std::string {
            ++sign_in_attempts;
            throw rym::SignInRequired("Signed out");
        },
        [&](std::string_view, rym::AccessReason reason) {
            require(reason == rym::AccessReason::sign_in, "Sign-in reason retained");
            ++sign_in_prompts;
        });
    throws([&] { signed_out.fetch("https://rateyourmusic.com/charts/top/song/all-time/2/"); });
    require(sign_in_attempts == 2 && sign_in_prompts == 1,
            "Closing sign-in without access cannot create a browser loop");
    reasons.clear();
    access_attempts = 0;
    rym::AccessTransport reversed(
        [&](std::string_view) {
            if (++access_attempts == 1)
                throw rym::SignInRequired("Sign-in first");
            if (access_attempts == 2)
                throw rym::VerificationRequired("Security check after sign-in");
            return std::string("chart");
        },
        [&](std::string_view, rym::AccessReason reason) { reasons.push_back(reason); });
    require(reversed.fetch("https://rateyourmusic.com/charts/top/song/all-time/2/") == "chart" &&
                reasons == std::vector<rym::AccessReason>{rym::AccessReason::sign_in,
                                                          rym::AccessReason::verification},
            "Sign-in followed by security verification retains independent retry limits");
    std::size_t timeout_fetches{}, timeout_prompts{};
    rym::AccessTransport timeout(
        [&](std::string_view) -> std::string {
            ++timeout_fetches;
            throw rym::SignInRequired("Sign-in");
        },
        [&](std::string_view, rym::AccessReason) {
            ++timeout_prompts;
            throw rym::Error("Manual access timed out");
        });
    throws([&] { timeout.fetch("https://rateyourmusic.com/charts/top/song/all-time/2/"); });
    require(timeout_fetches == 1 && timeout_prompts == 1,
            "Failed access handoff cannot issue another request");
    rym::cancelled.store(true);
    throws([&] { timeout.fetch("https://rateyourmusic.com/charts/top/song/all-time/2/"); });
    rym::cancelled.store(false);
    require(timeout_fetches == 1 && timeout_prompts == 1,
            "Cancellation prevents both navigation and access prompts");
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
    auto report = rym::compare_library(snapshot, first, music, true, {9, 10}, {85, 100});
    auto restored_report = rym::parse_report(rym::report_json(report));
    require(restored_report.songs[0].song.url == snapshot.songs[0].url && restored_report.offline,
            "JSON links/source");
    rym::write_json(temporary.path / "missing.json", rym::report_json(report));
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
    auto output = rym::PortablePaths::from_directory(temporary.path).report;
    auto args = std::vector<std::string>{
        "rym-crosscheck", "--music-dir", rym::path_text(music),     "--offline", "--top", "1",
        "--no-open",      "--cache-dir", rym::path_text(cache_path)};
    require(rym::application(args, temporary.path) == 0, "CLI offline result");
    require(rym::parse_report(rym::read_json(output)).songs[0].owned, "Owned songs excluded");
    for (const auto &workers : {"1", "8"}) {
        auto with_workers = args;
        with_workers.insert(with_workers.end(), {"--web-workers", workers});
        require(rym::application(with_workers, temporary.path) == 0,
                "Web-worker CLI boundaries accepted");
    }
    rym::atomic_write(music / "unidentified.mp3", "missing metadata");
    require(rym::application(args, temporary.path) == 2, "Incomplete scan keeps exit code two");
    require(!rym::parse_report(rym::read_json(output)).warnings.empty(),
            "Incomplete scan warning reaches the JSON report");
    auto previous = rym::read_file(output);
    args[5] = "2";
    require(rym::application(args, temporary.path) == 1, "Incomplete chart fails");
    require(rym::read_file(output) == previous, "Previous report preserved");
    args[5] = "1";
    auto cached_chart = rym::read_json(cache_path / "chart.json");
    cached_chart.as_object()["captured"] = 253402300800LL;
    rym::write_json(cache_path / "chart.json", cached_chart);
    require(rym::application(args, temporary.path) == 1 && rym::read_file(output) == previous,
            "Invalid offline context cannot publish a report the reviewer would reject");
    rym::cancelled.store(false);
}
void cache_failures() {
    Temp temporary;
    auto cache_path = temporary.path / "cache";
    auto chart = cache_path / "chart.json";
    {
        rym::Cache cache(cache_path);
        cache.save_snapshot({{song("Title", {"Artist"})}, rym::now_seconds()});
    }
    struct Writable {
        std::filesystem::path path;
        ~Writable() noexcept {
            SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        }
    };
    rym::Handle blocked(CreateFileW(chart.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(static_cast<bool>(blocked), "Chart held without delete sharing");
    throws([&] { rym::Cache clearing(cache_path, true); });
    require(std::filesystem::exists(chart), "Failed cache clearing preserves the blocked record");
    blocked = rym::Handle{};
    auto valid_chart = rym::read_json(chart);
    {
        rym::Cache cache(cache_path);
        for (auto captured : {-1LL, 253402300800LL}) {
            auto invalid_chart = valid_chart;
            invalid_chart.as_object()["captured"] = captured;
            rym::write_json(chart, invalid_chart);
            throws([&] { cache.load_snapshot(1); });
        }
        auto invalid_chart = valid_chart;
        invalid_chart.as_object()["source"] = "https://example.com/";
        rym::write_json(chart, invalid_chart);
        throws([&] { cache.load_snapshot(1); });
        invalid_chart = valid_chart;
        invalid_chart.as_object()["songs"].as_array()[0].as_object()["title"] =
            std::string("Title\0truncated", 15);
        rym::write_json(chart, invalid_chart);
        throws([&] { cache.load_snapshot(1); });
    }
    auto lock_file = temporary.path / "readonly.lock";
    rym::atomic_write(lock_file, "");
    Writable restore_lock{lock_file};
    require(SetFileAttributesW(lock_file.c_str(), FILE_ATTRIBUTE_READONLY),
            "Read-only lock fixture");
    bool permission_error{};
    try {
        rym::FileLock lock(lock_file);
    } catch (const rym::Error &failure) {
        permission_error =
            std::string_view(failure.what()).find("Windows error") != std::string_view::npos;
    }
    require(permission_error, "Lock permission errors are not reported as another running process");
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
        rym::write_json(temporary.path / "missing.json",
                        rym::report_json(
                            rym::compare_library(restored, scan, music, true, {9, 10}, {85, 100})));
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
    for (const auto &option :
         std::vector<std::vector<std::string>>{{"--output", "missing.md"},
                                               {"--whitelist", "whitelist.toml"},
                                               {"--web-mode", "browser"},
                                               {"--web-mode", "http"},
                                               {"--private-browser"},
                                               {"--persistent-browser"},
                                               {"--web-workers", "0"},
                                               {"--web-workers", "9"}}) {
        std::vector<std::string> args{
            "rym-crosscheck", "--music-dir", rym::path_text(temporary.path),
            "--no-open",      "--cache-dir", rym::path_text(cache),

            "--offline",      "--top",       "1"};
        args.insert(args.end(), option.begin(), option.end());
        require(rym::application(args, temporary.path) == 1,
                "Unsupported options and worker counts must fail parsing");
        require(!std::filesystem::exists(cache),
                "Invalid CLI must not initialize cache or retrieval");
        require(rym::read_file(report) == "previous report", "Invalid CLI must preserve report");
    }
}
int quiet_child(std::string_view event_handle) {
    auto event = reinterpret_cast<HANDLE>(std::stoull(std::string(event_handle)));
    SetEvent(event);
    char input{};
    DWORD count{};
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), &input, 1, &count, nullptr) || count)
        return 2;
    for (auto stream : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        auto handle = GetStdHandle(stream);
        DWORD mode{};
        if (GetFileType(handle) != FILE_TYPE_CHAR || GetConsoleMode(handle, &mode) ||
            !WriteFile(handle, "child diagnostic\n", 17, &count, nullptr) || count != 17)
            return 3;
    }
    return 0;
}
void quiet_processes() {
    Temp temporary;
    std::wstring module(32768, L'\0');
    auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    require(length && length < module.size(), "Locate child test executable");
    module.resize(length);
    auto parent_error = GetStdHandle(STD_ERROR_HANDLE);
    auto launch = [&] {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        rym::Handle event(CreateEventW(&security, TRUE, FALSE, nullptr));
        rym::Handle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(event && job &&
                    SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                            sizeof(limits)),
                "Own test child");
        auto command = L"\"" + module + L"\" --quiet-child " +
                       std::to_wstring(reinterpret_cast<std::uintptr_t>(event.get()));
        auto child = rym::create_quiet_process(module, std::move(command));
        if (!AssignProcessToJobObject(job.get(), child.process.get())) {
            TerminateProcess(child.process.get(), 1);
            throw std::runtime_error("Cannot assign test child to job");
        }
        require(ResumeThread(child.thread.get()) != static_cast<DWORD>(-1), "Resume test child");
        require(WaitForSingleObject(child.process.get(), 10000) == WAIT_OBJECT_0,
                "Quiet child finishes without reading terminal input");
        DWORD code{};
        require(GetExitCodeProcess(child.process.get(), &code) && !code,
                "Child diagnostics use null streams");
        require(WaitForSingleObject(event.get(), 0) == WAIT_TIMEOUT,
                "Unrelated inheritable event is excluded");
        throws([&] {
            auto absent = rym::create_quiet_process(temporary.path / "absent.exe", L"absent.exe");
        });
    };
    launch();
    DWORD before{};
    require(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "Count process handles");
    for (int run = 0; run < 5; ++run)
        launch();
    DWORD after{};
    require(GetProcessHandleCount(GetCurrentProcess(), &after) && after <= before,
            "Successful and failed quiet launches release handles");
    require(GetStdHandle(STD_ERROR_HANDLE) == parent_error, "Parent error stream is unchanged");
}
void report_review_tests();
void launcher_tests();
void browser_socket_tests();
int launcher_child(int argc, char **argv);
int launcher_parent(std::string_view destination);
int main(int argc, char **argv) {
    if (argc >= 3 && std::string_view(argv[1]) == "--launcher-child")
        return launcher_child(argc, argv);
    if (argc == 3 && std::string_view(argv[1]) == "--launcher-parent")
        return launcher_parent(argv[2]);
    if (argc == 3 && std::string_view(argv[1]) == "--quiet-child")
        return quiet_child(argv[2]);
    rym::install_cancellation();
    int failed{};
    for (const auto &[name, function] : std::vector<std::pair<std::string, std::function<void()>>>{
             {"normalization", normalization},
             {"matching", matching},
             {"indexed oracle", oracle},
             {"distance oracle", distance_oracle},
             {"HTML parsing", parsing},
             {"report review", report_review_tests},
             {"launcher", launcher_tests},
             {"chart acquisition", acquisition},
             {"browser reply ownership", browser_protocol},
             {"browser socket failures", browser_socket_tests},
             {"album workers", album_workers},
             {"tag priority", tag_priority},
             {"storage/scanning", storage_and_scan},
             {"CLI offline", cli_offline},
             {"cache failure paths", cache_failures},
             {"resource moves", resource_moves},
             {"browser process streams", quiet_processes},
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
