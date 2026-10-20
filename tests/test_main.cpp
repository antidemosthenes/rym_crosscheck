#include "rym_crosscheck/cli/application.hpp"
#include "rym_crosscheck/library/scanner.hpp"
#include "rym_crosscheck/matching/index.hpp"
#include "rym_crosscheck/report/report.hpp"
#include "rym_crosscheck/rym/chart.hpp"
#include "rym_crosscheck/rym/parser.hpp"
#include "rym_crosscheck/storage/whitelist.hpp"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <limits>
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
        require(left[i].track_id == right[i].track_id &&
                    left[i].credit_gap == right[i].credit_gap &&
                    left[i].version_gap == right[i].version_gap &&
                    left[i].artist_gap == right[i].artist_gap &&
                    left[i].section_gap == right[i].section_gap &&
                    left[i].compound_gap == right[i].compound_gap &&
                    left[i].annotation_gap == right[i].annotation_gap &&
                    left[i].diacritic_gap == right[i].diacritic_gap &&
                    left[i].notation_gap == right[i].notation_gap,
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
    for (auto title : {"Song (Lively Interlude)", "Song (Demolition)", "Song (Editorial)",
                       "Song (Liveé)", "Song (Live_Archive)", "Song (Club Mixé)"})
        require(rym::identity(title, {"Band"}).version.empty(),
                "Recording qualifiers must match complete words");
    require(!rym::identity("Song (Live at the Theatre)", {"Band"}).version.empty(),
            "Explicit recording labels retain their protection");
    require(rym::normalize("  “Heroes”  ", true).text == "heroes", "Title quotation marks");
    require(rym::normalize("\"Heroes\" (2017 Remaster)", true).text == "heroes",
            "Formatting and remaster normalization compose");
    require(rym::normalize("Weird Fishes / Arpeggi", true).text == "weird fishes/arpeggi" &&
                rym::normalize("Weird Fishes/ Arpeggi", true).text == "weird fishes/arpeggi",
            "Title slash spacing is insignificant");
    require(rym::normalize("Title (Including ‘Section’)", true).text == "title",
            "Included-section annotations yield a base title for scoring");
    for (auto title : {"Title (Including 'First' and 'Second')",
                       "Title [Including ‘First’, ‘Second’, and ‘Third’]",
                       "Title (Including \"First\" & 'Second')",
                       "Title (Including 'The Devil's Song' and 'Finale')"})
        require(rym::normalize(title, true).text == "title",
                "Quoted included sections support lists and internal apostrophes");
    for (auto title :
         {"Title (Including 'First' and Second)", "Title (Including 'First' and 'Second)",
          "Title (Including 'First',)", "Title (Including '' and 'Second')",
          "Title (Including 'First' live)", "Title (Including 'First' 'Second')"}) {
        require(rym::normalize(title, true).text == rym::normalize(title).text,
                "Malformed or unquoted included-section lists remain significant");
    }
    require(rym::normalize("Title (Part II)", true).text == "title (part ii)" &&
                rym::normalize("Title (Including Live Intro)", true).text ==
                    "title (including live intro)" &&
                rym::normalize("\"Love\" and \"Hate\"", true).text == "\"love\" and \"hate\"" &&
                rym::normalize("Don't Stop!", true).text == "don't stop!" &&
                rym::normalize("AC / DC").text == "ac / dc",
            "Meaningful title punctuation, sections, and artist names remain intact");
    require(rym::threshold("0.900000").numerator == 9, "Rational threshold");
    throws([] { rym::threshold("1.01"); });
    throws([] { rym::normalize(std::string(1, static_cast<char>(0xff))); });
    for (auto value : {"", "Café", "Cafe\xCC\x81", "Ｓｏｎｇ［未知］", "Song 🎵/Finale"}) {
        auto name = rym::normalize(value);
        require(rym::decode_utf8(name.text) == name.points,
                "Prepared UTF-8 decoding preserves normalized Unicode code points");
    }
    throws([] { rym::decode_utf8(std::string(1, static_cast<char>(0xc3))); });
    require(rym::edit_distance(U"kitten", U"sitting", 3) == 3, "Levenshtein reference");
    require(rym::edit_distance(U"kitten", U"sitting", 2) > 2, "Cutoff reference");
    require(rym::passes(rym::similarity(U"abcdefghij", U"abcdxfghij", {9, 10}), {9, 10}),
            "Inclusive boundary");
}
void matching_pipeline() {
    const std::vector<std::string> titles{"Heroes",
                                          "\"Heroes\"",
                                          "Café (Unlisted [Reference])",
                                          "Weird Fishes / Arpeggi",
                                          "Song (Including 'First' and 'Second')",
                                          "Song (Live) [Extended Version]",
                                          "Song [Radio Edit] (2015 Remaster)",
                                          "Song - Album Version",
                                          "Song (Bad]",
                                          "Song [Unknown] (Part II)",
                                          "Don't Stop!",
                                          "\"Love\" and \"Hate\""};
    const std::vector<std::vector<std::string>> credits{
        {"Artist"}, {"Artist", "Guest"}, {"Guest", "Artist"}, {"Café", "Cafe\xCC\x81"}};
    for (const auto &title : titles)
        for (const auto &artists : credits) {
            auto prepared = rym::identity(title, artists);
            require(rym::identity_key(title, artists) == prepared.key,
                    "Exact-only and review preparation must construct the same identity key");
            std::vector<rym::Track> tracks{track(title, artists)};
            rym::Index index(tracks);
            auto query = song(title, artists);
            require(index.owns(query) && index.owns(prepared),
                    "Standalone and prepared exact lookup have identical semantics");
            equal_candidates(index.suggest(query, {9, 10}, {85, 100}),
                             index.suggest(prepared, {9, 10}, {85, 100}));
            equal_candidates(index.suggest(prepared, {9, 10}, {85, 100}),
                             index.suggest(prepared, {9, 10}, {85, 100}, true));
        }
    for (const auto &title : titles) {
        throws([&] { rym::identity_key(title, {}); });
        throws([&] { rym::identity_key(title, {std::string(1, static_cast<char>(0xff))}); });
    }
    throws([] { rym::identity_key("", {"Band"}); });
    std::vector<std::string> oversized{std::string(40000, 'a'), std::string(40000, 'b')};
    throws([&] { rym::identity("Song", oversized); });
    throws([&] { rym::identity_key("Song", oversized); });
    std::vector<rym::Track> annotated{track("[Intro] \"Heroes\"", {"David Bowie"})};
    auto query = song("Heroes", {"David Bowie"});
    auto candidates = rym::Index(annotated).suggest(query, {9, 10}, {85, 100});
    require(candidates.size() == 1 && candidates[0].annotation_gap,
            "Quote normalization preserves words revealed by annotation removal");
    std::string artist(60000, 'a'), title(60000, 't');
    auto near_artist = artist, farther_artist = artist, near_title = title;
    near_artist[30000] = 'b';
    farther_artist[30000] = farther_artist[30001] = 'b';
    near_title[30000] = 'u';
    std::vector<rym::Track> long_tracks{track(title, {near_artist}, "second.flac"),
                                        track(near_title, {artist}, "first.flac"),
                                        track(near_title, {near_artist}, "third.flac"),
                                        track(title, {farther_artist}, "fourth.flac")};
    rym::Index long_index(long_tracks);
    auto long_query = song(title, {artist});
    auto ranked = long_index.suggest(long_query, {9, 10}, {85, 100});
    require(ranked.size() == 3 && ranked[0].track_id == 1 && ranked[1].track_id == 0 &&
                ranked[2].track_id == 2,
            "Ranking remains exact when mean-score cross products exceed 64 bits");
    equal_candidates(ranked, long_index.suggest(long_query, {9, 10}, {85, 100}, true));
}
void matching_examples() {
    struct Example {
        std::string artist, rym_title, local_title, rym_album, local_album;
        bool version_gap{}, section_gap{};
    };
    const std::vector<Example> examples{
        {"David Bowie", "\"Heroes\"", "Heroes", "\"Heroes\"", "Heroes"},
        {"King Crimson", "21st Century Schizoid Man (Including 'Mirrors')",
         "21st Century Schizoid Man", "In the Court of the Crimson King",
         "In the Court of the Crimson King", false, true},
        {"Radiohead", "Weird Fishes / Arpeggi", "Weird Fishes/ Arpeggi", "In Rainbows",
         "In Rainbows"},
        {"Depeche Mode", "Enjoy the Silence", "Enjoy The Silence [Single Version]", "Violator",
         "The Singles 86 - 98 (Disc 1)", true},
        {"Curtis Mayfield", "Move On Up", "Move on Up (Extended Version)", "Curtis", "Curtis",
         true},
        {"King Crimson",
         "The Court of the Crimson King (Including 'The Return of the Fire Witch' and 'The Dance "
         "of the Puppets')",
         "The Court Of the Crimson King", "In the Court of the Crimson King",
         "In the Court of the Crimson King", false, true}};
    for (const auto &example : examples) {
        std::vector<rym::Track> tracks{track(example.local_title, {example.artist})};
        tracks[0].album = example.local_album;
        auto query = song(example.rym_title, {example.artist});
        query.album = example.rym_album;
        rym::Index index(tracks);
        auto owned = !example.version_gap && !example.section_gap;
        require(index.owns(query) == owned, "Requested title-format ownership examples");
        auto candidates = index.suggest(query, {9, 10}, {85, 100});
        require(candidates.size() == 1 && candidates[0].version_gap == example.version_gap &&
                    candidates[0].section_gap == example.section_gap &&
                    candidates[0].title.numerator == candidates[0].title.denominator,
                "Normalized title scores and explicit version review");
        equal_candidates(candidates, index.suggest(query, {9, 10}, {85, 100}, true));
        std::vector<rym::Track> reverse{track(example.rym_title, {example.artist})};
        require(rym::Index(reverse).owns(song(example.local_title, {example.artist})) == owned,
                "The same title normalization applies to both sources");
        std::swap(query.album, tracks[0].album);
        require(index.owns(query) == owned, "Albums do not affect ownership");
    }
    std::vector<rym::Track> variants{
        track("Enjoy the Silence [Single Version]", {"Depeche Mode"}, "single.flac"),
        track("Enjoy the Silence (Album Version)", {"Depeche Mode"}, "album.flac"),
        track("Enjoy the Silence (Live)", {"Depeche Mode"}, "live.flac"),
        track("Enjoy the Silence (Remix)", {"Depeche Mode"}, "remix.flac")};
    rym::Index index(variants);
    require(index.unique_tracks() == variants.size(),
            "Recording versions remain distinct editions");
    auto query = song("Enjoy the Silence", {"Depeche Mode"});
    auto candidates = index.suggest(query, {9, 10}, {85, 100});
    require(!index.owns(query) && candidates.size() == 3 && candidates[0].version_gap &&
                candidates[1].version_gap,
            "Edition suggestions precede segmented recording reviews");
    require(!candidates[0].annotation_gap && !candidates[1].annotation_gap &&
                candidates[2].annotation_gap,
            "General annotation review remains distinct from the existing edition path");
    for (const auto &title : {"Enjoy the Silence", "Enjoy the Silence [Single Version]",
                              "Enjoy the Silence (Album Version)", "Enjoy the Silence (Live)"})
        for (auto cutoff : {rym::Fraction{0, 1}, rym::Fraction{85, 100}, rym::Fraction{1, 1}}) {
            auto request = song(title, {"Depeche Mode"});
            equal_candidates(index.suggest(request, {9, 10}, cutoff),
                             index.suggest(request, {9, 10}, cutoff, true));
        }
    require(index.owns(song("Enjoy the Silence (Single Version)", {"Depeche Mode"})),
            "Equivalent explicit edition labels match regardless of bracket style");
    auto same_version = index.suggest(song("Enjoy the Silence (Single Version)", {"Depeche Mode"}),
                                      {9, 10}, {85, 100});
    require(!same_version.empty() && !same_version[0].version_gap,
            "Same-version suggestions precede edition differences");
    std::vector<rym::Track> protected_versions{
        track("Enjoy the Silence (Live) [Single Version]", {"Depeche Mode"})};
    auto protected_review = rym::Index(protected_versions).suggest(query, {9, 10}, {85, 100});
    require(protected_review.size() == 1 && protected_review[0].annotation_gap &&
                protected_review[0].version_gap,
            "Segmentation exposes recording differences for review without asserting ownership");
    rym::Scan scan;
    scan.tracks = variants;
    auto report =
        rym::compare_library({{query}, rym::now_seconds()}, scan, ".", true, {9, 10}, {85, 100});
    auto restored = rym::parse_report(rym::report_json(report));
    require(!restored.songs[0].owned && restored.songs[0].candidates.size() == 3 &&
                restored.songs[0].candidates[0].version_gap,
            "Version review survives report generation and JSON round trips");
    auto exclusion = rym::Whitelist::parse("songs=[{title='Heroes',artist='David Bowie'}]");
    require(exclusion.contains(song("\"Heroes\"", {"David Bowie"})),
            "Artist/title whitelist uses the same normalization");
    auto single = rym::Whitelist::parse(
        "songs=[{title='Enjoy the Silence [Single Version]',artist='Depeche Mode'}]");
    require(!single.contains(query), "Version-specific whitelist does not exclude other versions");
}
void edition_review() {
    const std::vector<std::string> editions{"Single Version",
                                            "Album Version",
                                            "Extended Version",
                                            "Extended Mix",
                                            "Extended",
                                            "Radio Edit",
                                            "Radio Version",
                                            "Radio Mix",
                                            "Single Edit",
                                            "Single Mix",
                                            "Album Edit",
                                            "Album Mix",
                                            "Edit",
                                            "Edited Version",
                                            "Extended Edit",
                                            "Short Version",
                                            "Short Edit",
                                            "Long Version",
                                            "Long Edit",
                                            "Full Length Version",
                                            "Full-Length Version",
                                            "Full Length",
                                            "Full-Length",
                                            "Original Version",
                                            "Original Mix",
                                            "12\" Version",
                                            "12-Inch Version",
                                            "7\" Version",
                                            "7-Inch Version",
                                            "12\" Mix",
                                            "7\" Mix",
                                            "Mono Version",
                                            "Stereo Version",
                                            "Mono",
                                            "Stereo"};
    for (const auto &edition : editions) {
        auto qualified = "Move on Up (" + edition + ")";
        std::vector<rym::Track> local{track(qualified, {"Curtis Mayfield"})};
        auto plain = song("Move On Up", {"Curtis Mayfield"});
        rym::Index index(local);
        require(!index.owns(plain), "Edition differences never establish ownership");
        auto result = index.suggest(plain, {9, 10}, {85, 100});
        require(result.size() == 1 && result[0].version_gap && rym::passes(result[0].title, {1, 1}),
                "Explicit editions yield a scored base-title review");
        require(index.owns(song("Move On Up [" + edition + "]", {"Curtis Mayfield"})) &&
                    index.owns(song("Move On Up - " + edition, {"Curtis Mayfield"})),
                "Edition bracket and dash styles have one identity");
        std::vector<rym::Track> unqualified{track("Move on Up", {"Curtis Mayfield"})};
        auto reverse = rym::Index(unqualified)
                           .suggest(song(qualified, {"Curtis Mayfield"}), {9, 10}, {85, 100});
        require(reverse.size() == 1 && reverse[0].version_gap, "Edition review is symmetric");
        auto whitelist =
            rym::Whitelist::parse("songs=[{title='" + qualified + "',artist='Curtis Mayfield'}]");
        require(!whitelist.contains(plain), "Edition-specific whitelist identity stays exact");
    }
    const std::vector<std::string> labels{"",
                                          " (Extended Version)",
                                          " [Radio Edit]",
                                          " (Live)",
                                          " (Demo)",
                                          " (Club Remix)",
                                          " (Live) (Demo)",
                                          " (Live) [Single Version]",
                                          " (Extended Version) (Live)",
                                          " (Including 'Finale') (Extended Version)",
                                          " (Extended Version) (2015 Remaster)",
                                          " (Extended Version) [Single Version]",
                                          " (Live) (Demo) (Extended Version)"};
    std::vector<rym::Track> tracks;
    for (std::size_t i = 0; i < labels.size(); ++i)
        tracks.push_back(
            track("Move on Up" + labels[i], {"Curtis Mayfield"}, std::to_string(i) + ".flac"));
    for (int sparse = 0; sparse < 2; ++sparse) {
        rym::Index index(tracks);
        for (const auto &label : labels)
            for (auto artist : {rym::Fraction{0, 1}, {9, 10}, {1, 1}})
                for (auto title : {rym::Fraction{0, 1}, {85, 100}, {1, 1}}) {
                    auto query = song("Move on Up" + label, {"Curtis Mayfield"});
                    equal_candidates(index.suggest(query, artist, title),
                                     index.suggest(query, artist, title, true));
                }
        for (int i = 0; i < 100; ++i)
            tracks.push_back(track("Different " + std::to_string(i), {"Unrelated Artist"}));
    }
    auto identity = [](std::string_view title) { return rym::identity(title, {"Band"}); };
    require(identity("Song (Extended Version) (Live)").key ==
                identity("Song (Live) (Extended Version)").key,
            "Edit annotations cannot conceal recording evidence through ordering");
    require(identity("Song [Single Version] (Extended Version)").key !=
                identity("Song (Extended Version)").key,
            "Stacked edition labels retain all explicit evidence");
    require(rym::review_versions(identity("Song (Live) [Single Version]").version,
                                 identity("Song (Live) [Album Version]").version),
            "Edition review can retain the same explicit recording");
    for (auto left : {"Song (Live)", "Song (Live) (Demo)", "Song (Demo)", "Song (Club Remix)",
                      "Song (Live) [Single Version]", "Song (Extended Version) (Demo)",
                      "Song ( Live )", "Song [ Club Mix ]"}) {
        require(!rym::review_versions(identity(left).version, identity("Song").version),
                "Edition review cannot cross recording boundaries");
        std::vector<rym::Track> local{track(left, {"Band"})};
        auto review = rym::Index(local).suggest(song("Song", {"Band"}), {9, 10}, {0, 1});
        require(review.size() == 1 && review[0].annotation_gap && review[0].version_gap &&
                    !rym::Index(local).owns(song("Song", {"Band"})),
                "Equal segmented bases permit review across recording differences only");
    }
    require(!rym::review_versions(identity("Song (Live) (Demo)").version,
                                  identity("Song (Live)").version),
            "All protected qualifiers participate in compatibility");
    for (auto suffix : {" (Extended Universe)", " (Original Sin)", " (Single Ladies)",
                        " (Album of the Year)", " (Radio Waves)", " (Version 2)", " (Editions)"}) {
        auto name = std::string("Song") + suffix;
        require(identity(name).title.text == rym::normalize(name, true).text,
                "Unknown annotations and ordinary words remain significant");
    }
    for (auto title : {"Song (First (Live))", "Song [First [Demo]]", "Song (Part (Two))"})
        require(identity(title).title.text == rym::normalize(title, true).text,
                "Nested unknown annotations cannot lose surrounding title text");
    require(identity("Song (Including 'First (Intro)')").sections.size() > 0,
            "Balanced parentheses inside section names preserve section evidence");
    auto long_unknown = "Song (" + std::string(60000, 'x') + ")";
    require(identity(long_unknown).version.empty() &&
                identity(long_unknown).title.text == rym::normalize(long_unknown, true).text,
            "Long unknown labels remain significant without recursive pattern matching");
    auto nested = "Song " + std::string(12000, '(') + "unknown" + std::string(12000, ')');
    require(identity(nested).version.empty(), "Deep unknown annotations use bounded stack space");
}
void general_annotation_review() {
    auto parts = [](std::string_view title) {
        return rym::extract_annotations(rym::normalize(title).text);
    };
    for (auto title : {"Song (An unknown label)", "[Intro] Song", "Song (First [Second])",
                       "Song (One) [Two]", "Ｓｏｎｇ［未知］"}) {
        auto result = parts(title);
        require(result.annotated && result.base == "song" && !result.source.empty(),
                "Balanced annotation segmentation is independent of its vocabulary");
    }
    for (auto title : {"Song (Bad]", "Song [Bad)", "Song (Unclosed", "Song Closed)",
                       "Song (Valid) [Unclosed", "Song ([Bad)]"}) {
        auto result = parts(title);
        require(!result.annotated && result.base == result.source,
                "Malformed pairs cannot be partially removed");
    }
    require(parts("(Only an annotation)").base.empty(), "Empty bases cannot establish a match");
    const std::vector<std::pair<std::string, std::string>> examples{
        {"Move On Up", "Move on Up (Unlisted Archive Performance 2025)"},
        {"Song", "Song (Live)"},
        {"Song", "Song [Club Remix]"},
        {"Song (Demo)", "Song [Instrumental]"},
        {"Song (Part One)", "Song [Part Two]"},
        {"Song", "[Unlisted Intro] Song (Unknown [Nested]) [Outro]"},
        {"War Pigs", "War Pigs (Luke's Wall/Finale)"},
        {"Café", "Cafe\xCC\x81 [別録音]"},
        {"Song (Unknown label)", "Song [Unknown label]"}};
    for (const auto &[plain, annotated] : examples)
        for (bool reverse : {false, true}) {
            auto query = song(reverse ? annotated : plain, {"Band"});
            auto title = reverse ? plain : annotated;
            std::vector<rym::Track> tracks{track(title, {"Band"})};
            for (int sparse = 0; sparse < 2; ++sparse) {
                rym::Index index(tracks);
                auto result = index.suggest(query, {9, 10}, {1, 1});
                require(!index.owns(query) && result.size() == 1 && result[0].annotation_gap &&
                            rym::passes(result[0].title, {1, 1}),
                        "Exact segmented bases always produce review rather than ownership");
                equal_candidates(result, index.suggest(query, {9, 10}, {1, 1}, true));
                rym::Scan scan{tracks, {}, 0};
                auto report = rym::parse_report(rym::report_json(rym::compare_library(
                    {{query}, rym::now_seconds()}, scan, ".", false, {9, 10}, {1, 1})));
                require(!report.songs[0].owned && report.songs[0].candidates.size() == 1 &&
                            report.songs[0].candidates[0].annotation_gap,
                        "Generic annotation review survives report serialization");
                for (int i = 0; i < 30; ++i)
                    tracks.push_back(track("Different " + std::to_string(i), {"Other Artist"}));
            }
            auto whitelist =
                rym::Whitelist(std::vector<rym::WhitelistEntry>{{{}, {}, title, {"Band"}}});
            require(!whitelist.contains(query), "Generic segmentation cannot broaden exclusions");
        }
    for (auto title : {"Song (Bad]", "Song [Unclosed", "Another Song (Unknown Label)",
                       "Song-Unknown Label", "(Song)"}) {
        std::vector<rym::Track> local{track(title, {"Band"})};
        auto result = rym::Index(local).suggest(song("Song", {"Band"}), {9, 10}, {1, 1});
        require(result.empty(), "Generic review needs valid pairs and identical nonempty bases");
    }
    std::vector<rym::Track> unrelated{track("Song (Unknown Label)", {"Cover Artist"})};
    require(rym::Index(unrelated).suggest(song("Song", {"Band"}), {9, 10}, {1, 1}).empty(),
            "Annotation removal cannot bypass artist eligibility");
    std::vector<rym::Track> unchanged{track("Song (Unknown Label)", {"Band"})};
    rym::Index same(unchanged);
    require(same.owns(song("SONG (UNKNOWN LABEL)", {"Band"})),
            "A complete normalized identity takes priority over segmentation");
    require(
        !same.suggest(song("SONG (UNKNOWN LABEL)", {"Band"}), {9, 10}, {1, 1})[0].annotation_gap,
        "Identical annotations do not claim an annotation difference");
    std::vector<rym::Track> duplicates{track("Song - 2024 Remaster", {"Band"}, "a.flac"),
                                       track("Song", {"Band"}, "b.flac"),
                                       track("Song (2024 Remaster)", {"Band"}, "c.flac")};
    auto query = song("Song [Entirely Unknown Label]", {"Band"});
    for (bool reverse : {false, true}) {
        if (reverse)
            std::ranges::reverse(duplicates);
        rym::Index index(duplicates);
        auto result = index.suggest(query, {9, 10}, {1, 1});
        require(index.unique_tracks() == 1 && result.size() == 1 && result[0].annotation_gap,
                "Duplicate groups retain every source segmentation");
        equal_candidates(result, index.suggest(query, {9, 10}, {1, 1}, true));
    }
}
void matching_composition() {
    struct Example {
        std::string local_title, query_title;
        std::vector<std::string> local_artists, query_artists;
        bool credit{}, artist{}, section{}, compound{};
    };
    const std::vector<Example> examples{{"Move on Up (Extended Version)",
                                         "Move on Up (Including 'Finale')",
                                         {"Curtis Mayfield"},
                                         {"Curtis Mayfield"},
                                         false,
                                         false,
                                         true},
                                        {"Move on Up/Finale (Extended Version)",
                                         "Move on Up (Including 'Finale')",
                                         {"Curtis Mayfield"},
                                         {"Curtis Mayfield"},
                                         false,
                                         false,
                                         true,
                                         true},
                                        {"Move on Up (Extended Version)",
                                         "Move on Up",
                                         {"Curtis Mayfield", "Guest"},
                                         {"Curtis Mayfield"},
                                         true},
                                        {"Song (Extended Version)",
                                         "Song",
                                         {"The Silver Moon Orchestra"},
                                         {"Silver Moon"},
                                         false,
                                         true},
                                        {"Song/Finale (Extended Version)",
                                         "Song",
                                         {"Band", "Guest"},
                                         {"Band"},
                                         true,
                                         false,
                                         false,
                                         true}};
    for (const auto &example : examples) {
        std::vector<rym::Track> tracks{track(example.local_title, example.local_artists)};
        auto query = song(example.query_title, example.query_artists);
        for (int sparse = 0; sparse < 2; ++sparse) {
            rym::Index index(tracks);
            auto result = index.suggest(query, {9, 10}, {85, 100});
            require(!index.owns(query) && result.size() == 1 && result[0].version_gap &&
                        result[0].credit_gap == example.credit &&
                        result[0].artist_gap == example.artist &&
                        result[0].section_gap == example.section &&
                        result[0].compound_gap == example.compound,
                    "Independent review evidence composes without establishing ownership");
            equal_candidates(result, index.suggest(query, {9, 10}, {85, 100}, true));
            for (int i = 0; i < 20; ++i)
                tracks.push_back(track("Other " + std::to_string(i), {"Unrelated Artist"}));
        }
    }
    std::mt19937 random(7331);
    std::vector<rym::Track> tracks;
    for (int i = 0; i < 240; ++i) {
        auto title = "Song " + std::to_string(i % 47) + " summer morning";
        if (i % 3 == 0)
            title += " (Extended Version)";
        if (i % 5 == 0)
            title += " (Including 'Finale')";
        if (i % 7 == 0)
            title += " (Unlisted [Nested Reference])";
        if (i % 11 == 0)
            title += "/Finale";
        tracks.push_back(track(std::move(title), {"Artist " + std::to_string(i % 13)},
                               std::to_string(i) + ".flac"));
    }
    for (int permutation = 0; permutation < 3; ++permutation) {
        std::ranges::shuffle(tracks, random);
        rym::Index index(tracks);
        for (int i = 0; i < 120; ++i) {
            auto title = "Song " + std::to_string(random() % 47) + " summer morning";
            if (i % 3 == 0)
                title += " (Extended Version)";
            if (i % 7 == 0)
                title += " [Previously unseen annotation]";
            if (i % 11 == 0)
                title += "/Finale";
            auto query = song(std::move(title), {"Artist " + std::to_string(random() % 13)});
            auto artist = i % 3 == 0 ? rym::Fraction{0, 1} : rym::Fraction{9, 10};
            auto cutoff = i % 5 == 0 ? rym::Fraction{0, 1} : rym::Fraction{85, 100};
            equal_candidates(index.suggest(query, artist, cutoff),
                             index.suggest(query, artist, cutoff, true));
        }
    }
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
    require(suggestions.size() == 2 && suggestions[0].credit_gap &&
                suggestions[0].artist.numerator > 0 && !rym::passes(suggestions[0].artist, {9, 10}),
            "Credit-mismatch review retains the actual full-credit similarity");
    equal_candidates(suggestions, index.suggest(song("Alpha", {"Band"}), {9, 10}, {85, 100}, true));
    for (bool reverse : {false, true}) {
        std::vector<rym::Track> editions{track("Shared song", {"Band", "Guest"}, "z.flac"),
                                         track("Shared song", {"Guest", "Band"}, "a.flac"),
                                         track("Shared song", {"Guest", "Band"}, "b.flac")};
        if (reverse)
            std::ranges::reverse(editions);
        rym::Index grouped(editions);
        require(grouped.unique_tracks() == 1, "Reordered duplicate credits stay grouped");
        for (auto principal : {"Band", "Guest"}) {
            auto query = song("Shared song", {principal});
            auto review = grouped.suggest(query, {9, 10}, {85, 100});
            require(!grouped.owns(query) && review.size() == 1 && review[0].credit_gap &&
                        editions[review[0].track_id].path == "a.flac",
                    "Duplicate editions preserve every recorded principal for review");
            equal_candidates(review, grouped.suggest(query, {9, 10}, {85, 100}, true));
        }
    }
}
void compound_title_review() {
    for (bool reverse : {false, true}) {
        auto local = reverse ? "War Pigs" : "War Pigs/Luke's Wall";
        auto title = reverse ? "War Pigs / Luke’s Wall" : "War Pigs";
        std::vector<rym::Track> tracks{track(local, {"Black Sabbath"})};
        auto query = song(title, {"Black Sabbath"});
        rym::Index index(tracks);
        require(!index.owns(query), "A slash component never establishes ownership");
        auto candidates = index.suggest(query, {9, 10}, {85, 100});
        require(candidates.size() == 1 && candidates[0].compound_gap &&
                    !rym::passes(candidates[0].title, {85, 100}),
                "War Pigs remains reviewable below the ordinary title threshold");
        auto expected =
            rym::similarity(rym::identity(title, {"Black Sabbath"}).title.points,
                            rym::identity(local, {"Black Sabbath"}).title.points, {0, 1});
        require(candidates[0].title.numerator == expected.numerator &&
                    candidates[0].title.denominator == expected.denominator,
                "Component reviews retain the actual full-title similarity");
        equal_candidates(candidates, index.suggest(query, {9, 10}, {85, 100}, true));
        rym::Scan scan{tracks, {}, 0};
        auto report = rym::parse_report(rym::report_json(rym::compare_library(
            {{query}, rym::now_seconds()}, scan, ".", false, {9, 10}, {85, 100})));
        require(!report.songs[0].owned && report.songs[0].candidates.size() == 1 &&
                    report.songs[0].candidates[0].compound_gap,
                "Compound-title review survives report serialization");
    }
    for (auto local :
         {"War Pigs/Luke's Wall (Live)", "War Pigs (Live)/Luke's Wall", "Not War Pigs/Luke's Wall",
          "War Pigs//Luke's Wall", "War Pigs/", "War Pigs/(Finale]"}) {
        std::vector<rym::Track> tracks{track(local, {"Black Sabbath"})};
        require(rym::Index(tracks)
                    .suggest(song("War Pigs", {"Black Sabbath"}), {9, 10}, {1, 1})
                    .empty(),
                "Component review respects recording labels and complete title boundaries");
    }
    for (auto title : {"1/2", "AC/DC", "Song (War Pigs/Finale)"}) {
        std::vector<rym::Track> tracks{track(title, {"Band"})};
        for (auto query : {"1", "AC", "War Pigs"})
            require(rym::Index(tracks).suggest(song(query, {"Band"}), {9, 10}, {85, 100}).empty(),
                    "Numeric, tiny, and parenthesized fragments do not create title reviews");
    }
    std::vector<rym::Track> tracks{
        track("War Pigs/Luke's Wall", {"Unrelated Cover Band"}),
        track("War Pigs/Luke's Wall", {"Black Sabbath", "Guest"}, "credits.mp3"),
        track("War Pigs/Luke's Wall", {"Black Sabbath"}, "duplicate-b.mp3"),
        track("War Pigs / Luke's Wall", {"Black Sabbath"}, "duplicate-a.mp3"),
        track("War Pigs/Finale", {"Black Sabbath"}, "other.mp3"),
        track("Été/Finale", {"Band"}, "unicode.mp3")};
    for (int i = 0; i < 80; ++i)
        tracks.push_back(track("Other " + std::to_string(i), {"Black Sabbath"},
                               "other-" + std::to_string(i) + ".mp3"));
    for (int sparse = 0; sparse < 2; ++sparse) {
        rym::Index index(tracks);
        for (auto query :
             {song("War Pigs", {"Black Sabbath"}), song("Luke's Wall", {"Black Sabbath"}),
              song("War Pigs/Luke's Wall", {"Black Sabbath"}), song("Été", {"Band"})})
            for (auto title_cutoff : {rym::Fraction{0, 1}, {85, 100}, {1, 1}}) {
                auto candidates = index.suggest(query, {9, 10}, title_cutoff);
                require(!candidates.empty(), "Both retrieval paths retain compound reviews");
                equal_candidates(candidates, index.suggest(query, {9, 10}, title_cutoff, true));
            }
        auto candidates = index.suggest(song("War Pigs", {"Black Sabbath"}), {9, 10}, {85, 100});
        require(candidates.size() == 3 && candidates[1].track_id == 3 && candidates[2].credit_gap,
                "Compound candidates deduplicate editions and retain credit-mismatch labels");
        for (int i = 0; i < 400; ++i)
            tracks.push_back(track("Unrelated " + std::to_string(i), {"Other Artist"}));
    }
    std::vector<rym::Track> same{track("Weird Fishes/ Arpeggi", {"Radiohead"})};
    require(rym::Index(same).owns(song("Weird Fishes / Arpeggi", {"Radiohead"})),
            "Slash spacing still permits exact ownership");
}
void dash_title_review() {
    for (const auto &compound :
         {"Disorder - 2019 Digital Master", "Disorder – 2019 Digital Master",
          "Disorder — 2019 Digital Master", "Disorder - Previously Unknown Label",
          "Disorder [Notes] - 2019 Digital Master", "\"Disorder\" - 2019 Digital Master",
          "Intro - Disorder - Finale", "Disorder - 2019 Digital Master/Bonus"})
        for (bool reverse : {false, true}) {
            std::vector<rym::Track> tracks{
                track(reverse ? "Disorder" : compound, {"Band"}, "target.flac")};
            auto query = song(reverse ? compound : "Disorder", {"Band"});
            for (int sparse = 0; sparse < 2; ++sparse) {
                for (std::size_t i = tracks.size(); i < (sparse ? 500u : 80u); ++i)
                    tracks.push_back(track("Unrelated title " + std::to_string(i),
                                           {sparse ? "Other Artist" : "Band"},
                                           std::to_string(i) + ".flac"));
                rym::Index index(tracks);
                for (auto cutoff : {rym::Fraction{85, 100}, rym::Fraction{1, 1}}) {
                    auto result = index.suggest(query, {9, 10}, cutoff);
                    require(!index.owns(query) && result.size() == 1 &&
                                result[0].track_id == 0 && result[0].compound_gap &&
                                result[0].annotation_gap ==
                                    (std::string_view(compound).find('[') != std::string_view::npos),
                            "Spaced-dash components produce review after normalization and block removal");
                    equal_candidates(result, index.suggest(query, {9, 10}, cutoff, true));
                }
            }
        }
    for (auto title :
         {"Disorderly - 2019 Digital Master", "Not Disorder - 2019 Digital Master",
          "Disorder-2019 Digital Master", "Disorder - - 2019 Digital Master",
          "Disorder - [Unbalanced", "Other (Disorder - 2019 Digital Master)",
          "Disorder - Live", "Disorder (Live) - 2019 Digital Master",
          "Disorder - 2019 Digital Master (Live)"}) {
        std::vector<rym::Track> tracks{track(title, {"Band"})};
        rym::Index index(tracks);
        auto query = song("Disorder", {"Band"});
        auto result = index.suggest(query, {9, 10}, {1, 1});
        require(result.empty(),
                "Dash review rejects partial words, unspaced dashes, empty parts, and recording conflicts");
        equal_candidates(result, index.suggest(query, {9, 10}, {1, 1}, true));
    }
    for (auto title : {"2019", "AC"}) {
        std::vector<rym::Track> tracks{track("Disorder - 2019", {"Band"}),
                                       track("AC - DC", {"Band"})};
        require(rym::Index(tracks).suggest(song(title, {"Band"}), {9, 10}, {1, 1}).empty(),
                "Numeric and short dash components cannot establish review evidence");
    }
    auto query = song("Disorder", {"Band"});
    std::vector<rym::Track> unrelated{
        track("Disorder - 2019 Digital Master", {"Other Artist"})};
    require(rym::Index(unrelated).suggest(query, {9, 10}, {85, 100}).empty(),
            "Dash review still requires eligible artist credits");
    std::vector<rym::Track> local{track("Disorder - 2019 Digital Master", {"Band"})};
    require(rym::Index(local).owns(song("DISORDER — 2019 DIGITAL MASTER", {"Band"})),
            "Equivalent complete dash titles still establish exact ownership");
    require(!rym::Whitelist::parse("songs=[{title='Disorder',artist='Band'}]")
                 .contains(song(local[0].title, {"Band"})),
            "Dash review does not broaden exact whitelist exclusions");
    rym::Scan scan{local, {}, 0};
    auto report = rym::parse_report(rym::report_json(rym::compare_library(
        {{query}, rym::now_seconds()}, scan, ".", false, {9, 10}, {85, 100})));
    require(!report.songs[0].owned && report.songs[0].candidates.size() == 1 &&
                report.songs[0].candidates[0].compound_gap &&
                report.songs[0].candidates[0].title_score.numerator <
                    report.songs[0].candidates[0].title_score.denominator,
            "Disorder Digital Master remains visible for review after report serialization");
}
void artist_group_boundaries() {
    auto check = [](std::vector<rym::Track> tracks,
                    const std::vector<std::pair<std::vector<std::string>, std::string>> &queries) {
        for (bool reverse : {false, true}) {
            if (reverse)
                std::ranges::reverse(tracks);
            rym::Index index(tracks);
            for (const auto &[artists, path] : queries) {
                auto query = song("Credit boundary test", artists);
                auto result = index.suggest(query, {1, 1}, {1, 1});
                require(!index.owns(query) && result.size() == 1 && result[0].diacritic_gap &&
                            tracks[result[0].track_id].path == rym::wide(path),
                        "Latin credit equality preserves complete structured artist boundaries");
                equal_candidates(result, index.suggest(query, {1, 1}, {1, 1}, true));
            }
        }
    };
    check({track("Credit boundary test", {"Bôa | Guest"}, "literal.flac"),
           track("Credit boundary test", {"Bôa", "Guest"}, "structured.flac")},
          {{{"Boa | Guest"}, "literal.flac"}, {{"Boa", "Guest"}, "structured.flac"}});
    check({track("Credit boundary test", {"Á", "B | C"}, "first.flac"),
           track("Credit boundary test", {"A | B", "Ć"}, "second.flac")},
          {{{"A", "B | C"}, "first.flac"}, {{"A | B", "C"}, "second.flac"}});
}
void title_notation_review() {
    auto prepare = [](std::string_view title) {
        return rym::review_title(rym::normalize(title).text);
    };
    for (const auto &[original, expected] : std::vector<std::pair<std::string, std::string>>{
             {"Song Pt.3", "song part 3"}, {"Song PT 3", "song part 3"},
             {"Song Pť.3", "song part 3"},
             {"Song Part.3", "song part 3"}, {"Song Vol.03", "song volume 03"},
             {"Song Volume03", "song volume 03"}, {"Song No.3", "song number 3"},
             {"Song Number. 3", "song number 3"},
             {"Song Pt.3 Vol.02 No.1", "song part 3 volume 02 number 1"},
             {"Étude Pt.3 feat. Guest", "etude part 3"}, {"が Pt.3", "が part 3"},
             {"ø Pt.3", "ø part 3"}, {"Song Pt.3 feat. Vol.2", "song part 3"}}) {
        auto result = prepare(original);
        require(result && result->notation && result->whole.text == expected &&
                    result->whole.points == rym::decode_utf8(expected),
                "Review normalization expands only supported numbered labels and preserves digits");
    }
    for (auto marker :
         {"feat", "feat.", "ft", "ft.", "feature", "feature.", "featuring", "featuring."})
        for (auto boundary : {" ", " - ", " (", " ["}) {
            auto title = std::string("Song Pt.3") + boundary + marker + " Guest";
            if (std::string_view(boundary).ends_with('('))
                title += ')';
            else if (std::string_view(boundary).ends_with('['))
                title += ']';
            auto result = prepare(title);
            require(result && result->notation && result->whole.text == "song part 3",
                    "Trailing title credits share a parser for all supported markers and boundaries");
        }
    for (auto title : {"Song Part 3", "Song Volume 03", "Song Number 3", "Song Pt. III",
                       "Song Vol. Three", "Song No.Ⅲ", "Song Apt.3", "Song Pt.3a", "Song Volta3",
                       "Song feat.", "Song (feat.)", "feat. Guest", "Song featurette Guest",
                       "Song Pt.3 [Bad)", "Song Pt.3 (Unclosed", "Song Pt.3 Closed)"})
        require(!prepare(title), "Unsupported or malformed notation remains unchanged");
    auto folded = prepare("Étude Part 3");
    require(folded && folded->diacritics && !folded->notation &&
                folded->whole.text == "etude part 3",
            "Latin folding precedes notation without inventing an abbreviation reason");
    const std::vector<std::pair<std::string, std::string>> examples{
        {"Luv (Sic.) Part 3", "Luv (Sic.) Pt.3 feat. Shing02"},
        {"Song Part 3", "Song Pt.3"}, {"Song Volume 03", "Song Vol.03"},
        {"Song Part 3", "Song Pť.3"},
        {"Song Number 3", "Song No.3"}, {"Song Part 3", "Song Part 3 feat. Guest"},
        {"Étude Part 3", "Etude Pt.3"}, {"Song Part 3", "Song Pt.3 [Unknown notes]"},
        {"Song Part 3", "Intro/Song Pt.3/Finale"},
        {"Song Part 3", "Intro - Song Pt.3 - Finale"},
        {"Sòng Part 3", "Intro/Song Pt.3 [Notes]/Finale"},
        {"Song Part 3 (Extended Version)", "Song Pt.3"}};
    for (const auto &[plain, variant] : examples)
        for (bool reverse : {false, true}) {
            std::vector<rym::Track> tracks{
                track(reverse ? plain : variant, {"Nujabes"}, "target.flac")};
            auto query = song(reverse ? variant : plain, {"Nujabes"});
            for (int sparse = 0; sparse < 2; ++sparse) {
                for (std::size_t i = tracks.size(); i < (sparse ? 240u : 40u); ++i)
                    tracks.push_back(track("Unrelated title " + std::to_string(i),
                                           {sparse ? "Other Artist" : "Nujabes"},
                                           std::to_string(i) + ".flac"));
                rym::Index index(tracks);
                auto result = index.suggest(query, {9, 10}, {1, 1});
                require(!index.owns(query) && result.size() == 1 && result[0].track_id == 0 &&
                            result[0].notation_gap,
                        "Ordered review forms preserve ownership rules in both retrieval paths");
                equal_candidates(result, index.suggest(query, {9, 10}, {1, 1}, true));
                auto query_identity = rym::identity(query.title, query.artists);
                auto local_identity = rym::identity(tracks[0].title, tracks[0].artists);
                auto expected = rym::similarity(query_identity.title.points,
                                                local_identity.title.points, {0, 1});
                require(result[0].title.numerator == expected.numerator &&
                            result[0].title.denominator == expected.denominator,
                        "Notation reviews retain scores from the original canonical titles");
            }
            require(!rym::Whitelist::parse("songs=[{artist='Nujabes',title='" + plain + "'}]")
                         .contains(song(variant, {"Nujabes"})),
                    "Notation normalization cannot broaden artist-title whitelist exclusions");
        }
    for (auto local : {"Song Pt.4", "Song Pt.03", "Song Pt.III", "Song Pt.3a",
                       "Song Pt.3 [Broken", "Song Pt.3 (Live)", "Song Pt.3 (Club Remix)",
                       "Song Pt.3 extra words"}) {
        std::vector<rym::Track> tracks{track(local, {"Nujabes"})};
        rym::Index index(tracks);
        auto query = song("Song Part 3", {"Nujabes"});
        auto result = index.suggest(query, {9, 10}, {1, 1});
        require(result.empty(), "Notation review requires equality and compatible recording evidence");
        equal_candidates(result, index.suggest(query, {9, 10}, {1, 1}, true));
    }
    for (auto local : {"Song (feat. Guest)", "Song - Pt.3"}) {
        std::vector<rym::Track> tracks{track(local, {"Nujabes"})};
        auto result = rym::Index(tracks).suggest(song("Song", {"Nujabes"}), {9, 10}, {1, 1});
        require(result.size() == 1 && !result[0].notation_gap,
                "Existing annotation and component evidence keeps its original reasons");
    }
    const std::string long_title = "A very long and distinctive song title ";
    std::vector<rym::Track> similar{track(long_title + "Pt.3", {"Nujabes"})};
    auto ordinary = rym::Index(similar).suggest(song(long_title + "Part 3", {"Nujabes"}),
                                              {9, 10}, {85, 100});
    require(ordinary.size() == 1 && !ordinary[0].notation_gap,
            "Ordinary fuzzy eligibility does not acquire a notation flag");
    std::vector<rym::Track> local{track("Luv (Sic.) Pt.3 feat. Shing02", {"Nujabes"})};
    auto query = song("Luv (Sic.) Part 3", {"Nujabes"});
    rym::Scan scan{local, {}, 0};
    auto report = rym::parse_report(rym::report_json(rym::compare_library(
        {{query}, rym::now_seconds()}, scan, ".", false, {9, 10}, {85, 100})));
    require(!report.songs[0].owned && report.songs[0].candidates.size() == 1 &&
                report.songs[0].candidates[0].notation_gap &&
                !report.songs[0].candidates[0].annotation_gap &&
                !rym::passes(report.songs[0].candidates[0].title_score, {85, 100}),
            "The Nujabes example remains visible with truthful scores after report serialization");
    local[0].artists = {"Other Artist"};
    require(rym::Index(local).suggest(query, {9, 10}, {1, 1}).empty(),
            "Title notation cannot bypass artist eligibility");
    local[0].artists = {"Nujabes", "Shing02"};
    auto collaboration = rym::Index(local).suggest(query, {9, 10}, {1, 1});
    require(collaboration.size() == 1 && collaboration[0].credit_gap &&
                collaboration[0].notation_gap,
            "Structured credit mismatches retain separate title-notation evidence");
    equal_candidates(collaboration, rym::Index(local).suggest(query, {9, 10}, {1, 1}, true));
    std::vector<rym::Track> different_parts{
        track("Intro Pt.3/Finale", {"Nujabes"})};
    require(rym::Index(different_parts)
                .suggest(song("Intro Part 3/Outro", {"Nujabes"}), {9, 10}, {1, 1})
                .empty(),
            "Shared components cannot make two different complete compound titles equal");
    std::vector<rym::Track> collisions{track("aabaaa part 3", {"Nujabes"})};
    rym::Index collision_index(collisions);
    auto collision_query = song("aaabaa pt.3", {"Nujabes"});
    auto collision_result = collision_index.suggest(collision_query, {9, 10}, {1, 1});
    require(collision_result.empty(), "Equal trigram counts do not prove prepared-title equality");
    equal_candidates(collision_result,
                     collision_index.suggest(collision_query, {9, 10}, {1, 1}, true));
    std::vector<rym::Track> ranked{
        track("Song Pt.3 feat. Guest", {"Nujabes"}, "z.flac"),
        track("Song Pt.3 feat. Guest", {"Nujabes"}, "a.flac"),
        track("Song Pt 3 feat. Other Guest", {"Nujabes"}, "b.flac"),
        track("Song Part 3 featuring Third Guest", {"Nujabes"}, "c.flac")};
    std::vector<std::filesystem::path> expected_paths;
    for (int order = 0; order < 2; ++order) {
        if (order)
            std::ranges::reverse(ranked);
        rym::Index index(ranked);
        auto result = index.suggest(song("Song Part 3", {"Nujabes"}), {9, 10}, {1, 1});
        require(result.size() == 3 && index.unique_tracks() == 3,
                "Notation suggestions retain three distinct canonical recordings");
        std::vector<std::filesystem::path> paths;
        for (const auto &candidate : result)
            paths.push_back(ranked[candidate.track_id].path);
        if (order)
            require(paths == expected_paths, "Notation ranking is independent of insertion order");
        else
            expected_paths = std::move(paths);
        equal_candidates(result,
                         index.suggest(song("Song Part 3", {"Nujabes"}), {9, 10}, {1, 1}, true));
    }
    const std::vector<std::string> forms{
        "Song Part 3", "Song Pt.3 feat. Guest", "Song Pt.03", "Song Part 03",
        "Song Pt.4 [Notes]", "Song (feat. Guest)", "Song", "Song Pt.3 (Live)",
        "Étude Vol.2", "Etude Volume 2", "Intro/Song Pt.3/Finale",
        "Intro - Song Part 3 - Outro", "Song Number 3", "Song No.3", "Song Pt.III"};
    std::vector<rym::Track> varied;
    for (std::size_t i = 0; i < 160; ++i)
        varied.push_back(track(forms[i % forms.size()], {"Artist " + std::to_string(i % 7)},
                               std::to_string(i) + ".flac"));
    for (int order = 0; order < 2; ++order) {
        if (order)
            std::ranges::reverse(varied);
        rym::Index index(varied);
        for (std::size_t i = 0; i < 55; ++i)
            for (auto cutoff : {rym::Fraction{0, 1}, rym::Fraction{85, 100}, rym::Fraction{1, 1}}) {
                auto request = song(forms[i % forms.size()], {"Artist " + std::to_string(i % 7)});
                equal_candidates(index.suggest(request, {9, 10}, cutoff),
                                 index.suggest(request, {9, 10}, cutoff, true));
            }
    }
}
void normalized_component_review() {
    for (const auto &[single, compound] :
         std::vector<std::pair<std::string, std::string>>{
             {"War Pigs", "War Pigs [Bonus material]/Luke's Wall"},
             {"War Pigs [Other notes]", "War Pigs [Bonus material]/Luke's Wall"},
             {"War Pigs", "War Pigs [Nested (unknown annotation)]/Luke's Wall [Finale]"},
             {"Été", "Été [Notes inconnues]/Finale"},
             {"War Pigs", "\"War Pigs\"/\"Luke's Wall\""},
             {"War Pigs", "Intro/War Pigs/Finale/Encore/Outro"},
             {"War Pigs", "War Pigs [One [Two [Three [Four [Five [Six [Seven [Eight [Nine]]]]]]]]]/Finale"}})
        for (bool reverse : {false, true}) {
            std::vector<rym::Track> tracks{
                track(reverse ? single : compound, {"Band"}, "target.flac")};
            auto query = song(reverse ? compound : single, {"Band"});
            auto annotation = compound.find('[') != compound.npos;
            for (int sparse = 0; sparse < 2; ++sparse) {
                for (std::size_t i = tracks.size(); i < (sparse ? 500u : 80u); ++i)
                    tracks.push_back(track("Unrelated title " + std::to_string(i),
                                           {sparse ? "Other Artist" : "Band"},
                                           std::to_string(i) + ".flac"));
                rym::Index index(tracks);
                auto result = index.suggest(query, {9, 10}, {1, 1});
                require(!index.owns(query) && result.size() == 1 && result[0].track_id == 0 &&
                            result[0].compound_gap && result[0].annotation_gap == annotation &&
                            result[0].title.numerator < result[0].title.denominator,
                        "Annotation removal and quote formatting precede whole-component review");
                equal_candidates(result, index.suggest(query, {9, 10}, {1, 1}, true));
                auto single_name = rym::identity(single, {"Band"});
                auto compound_name = rym::identity(compound, {"Band"});
                auto shorter = annotation ? rym::normalize(single_name.annotations.annotated
                                                              ? single_name.annotations.base
                                                              : single_name.title.text)
                                          : single_name.title;
                auto longer = annotation ? rym::normalize(compound_name.annotations.base)
                                         : compound_name.title;
                require(result[0].title.numerator == shorter.points.size() &&
                            result[0].title.denominator == longer.points.size(),
                        "Component reviews score the actual selected complete comparison forms");
            }
        }
    for (auto title : {"War Pigs Forever/Finale", "Not War Pigs/Finale",
                       "War Pigs [Unbalanced)/Finale", "War Pigs [Bonus]//Finale",
                       "War Pigs (Live)/Finale", "War Pigs [Bonus]/Finale (Live)"}) {
        std::vector<rym::Track> tracks{track(title, {"Band"})};
        rym::Index index(tracks);
        auto query = song("War Pigs", {"Band"});
        require(index.suggest(query, {9, 10}, {1, 1}).empty(),
                "Normalized components reject substrings, malformed input, and recording conflicts");
        equal_candidates(index.suggest(query, {9, 10}, {1, 1}),
                         index.suggest(query, {9, 10}, {1, 1}, true));
    }
    std::vector<rym::Track> tracks{track("War Pigs [Bonus]/Finale", {"Band"})};
    rym::Scan scan{tracks, {}, 0};
    auto report = rym::parse_report(rym::report_json(rym::compare_library(
        {{song("War Pigs", {"Band"})}, rym::now_seconds()}, scan, ".", false,
        {9, 10}, {1, 1})));
    require(!report.songs[0].owned && report.songs[0].candidates.size() == 1 &&
                report.songs[0].candidates[0].compound_gap &&
                report.songs[0].candidates[0].annotation_gap,
            "Normalized component reasons survive report round trips");
}
void included_section_review() {
    const std::string annotated = "The Court of the Crimson King (Including 'The Return of the "
                                  "Fire Witch' and 'The Dance of the Puppets')";
    auto request = song(annotated, {"King Crimson"});
    std::vector<rym::Track> local{
        track("The Court Of the Crimson King", {"King Crimson"}, "court.flac")};
    rym::Index index(local);
    require(!index.owns(request), "Included-section differences cannot prove ownership");
    auto candidates = index.suggest(request, {9, 10}, {85, 100});
    require(candidates.size() == 1 && candidates[0].section_gap &&
                candidates[0].title.numerator == candidates[0].title.denominator,
            "Perfect base-title similarity still requires section review");
    rym::Scan scan;
    scan.tracks = local;
    auto report = rym::parse_report(rym::report_json(rym::compare_library(
        {{request}, rym::now_seconds()}, scan, ".", true, {9, 10}, {85, 100})));
    require(!report.songs[0].owned && report.songs[0].candidates.size() == 1 &&
                report.songs[0].candidates[0].section_gap &&
                report.songs[0].song.title == annotated,
            "The Court remains visible for review after report round trips");
    auto exclusion = rym::Whitelist::parse(
        "songs=[{title='The Court Of the Crimson King',artist='King Crimson'}]");
    require(!exclusion.contains(request),
            "Short-title whitelist entries do not erase section differences");
    require(rym::Whitelist(std::vector<rym::WhitelistEntry>{{{}, request.url, {}, {}}})
                .contains(request),
            "An explicit song-URL exclusion still applies");
    std::vector<rym::Track> editions{
        track("Song (Including 'First' and 'Second')", {"Band"}, "a.flac"),
        track("Song", {"Band"}, "base.flac"),
        track("Song (Including 'Different')", {"Band"}, "different.flac")};
    rym::Index variants(editions);
    require(variants.unique_tracks() == 3, "Section differences keep distinct local editions");
    auto same = song("Song [Including \"First\", \"Second\"]", {"Band"});
    require(variants.owns(same), "Formatting-only differences in the same section list are exact");
    auto result = variants.suggest(same, {9, 10}, {85, 100});
    require(result.size() == 3 && !result[0].section_gap && result[1].section_gap &&
                result[2].section_gap,
            "Suggestions prefer matching section evidence without hiding review alternatives");
    for (const auto &title :
         {"Song", "Song (Including 'First' and 'Second')", "Song (Including 'Second' and 'First')",
          "Song (Including 'Different')"})
        for (auto cutoff : {rym::Fraction{0, 1}, rym::Fraction{85, 100}, rym::Fraction{1, 1}}) {
            auto query = song(title, {"Band"});
            equal_candidates(variants.suggest(query, {9, 10}, cutoff),
                             variants.suggest(query, {9, 10}, cutoff, true));
        }
    std::vector<rym::Track> live{track("Song (Live)", {"Band"})};
    auto review =
        rym::Index(live).suggest(song("Song (Including 'First')", {"Band"}), {9, 10}, {85, 100});
    require(review.size() == 1 && review[0].annotation_gap && review[0].version_gap &&
                review[0].section_gap,
            "Exact segmented bases expose recording and section differences together");
}
void artist_credit_overlap() {
    auto query = song("All Along the Watchtower", {"The Jimi Hendrix Experience"});
    query.album = "Electric Ladyland";
    std::vector<rym::Track> tracks{
        track("All Along the Watchtower", {"Jimi Hendrix"}, "hendrix.flac"),
        track("All Along the Watchtower", {"Bob Dylan"}, "other.flac"),
        track("All Along the Watchtower", {"Jimi Hendrix"}, "z-duplicate.mp3")};
    tracks[0].album = "Different compilation";
    rym::Index index(tracks);
    auto candidates = index.suggest(query, {9, 10}, {85, 100});
    require(!index.owns(query) && candidates.size() == 1 && candidates[0].track_id == 0 &&
                candidates[0].artist_gap && !candidates[0].credit_gap &&
                candidates[0].artist.numerator > 0 && !rym::passes(candidates[0].artist, {9, 10}) &&
                candidates[0].title.numerator == candidates[0].title.denominator,
            "Exact title plus expanded artist credit is review-only with honest text scores");
    equal_candidates(candidates, index.suggest(query, {9, 10}, {85, 100}, true));
    require(
        index.suggest(song("All Along the Watchtowxr", query.artists), {9, 10}, {85, 100}).empty(),
        "Artist-overlap fallback requires an exact normalized title");
    auto live_review =
        index.suggest(song("All Along the Watchtower (Live)", query.artists), {9, 10}, {85, 100});
    require(live_review.size() == 1 && live_review[0].artist_gap && live_review[0].annotation_gap &&
                live_review[0].version_gap,
            "Artist overlap and exact segmented bases retain separate review reasons");
    auto exclusions =
        rym::Whitelist::parse("songs=[{title='All Along the Watchtower',artist='Jimi Hendrix'}]");
    require(!exclusions.contains(query),
            "Artist overlap never broadens exact whitelist identities");
    rym::Scan scan;
    scan.tracks = tracks;
    auto report = rym::parse_report(rym::report_json(
        rym::compare_library({{query}, rym::now_seconds()}, scan, ".", false, {9, 10}, {85, 100})));
    require(!report.songs[0].owned && report.songs[0].candidates.size() == 1 &&
                report.songs[0].candidates[0].artist_gap,
            "Artist-overlap review survives comparison and report round trips");
    for (const auto &[short_name, long_name] : std::vector<std::pair<std::string, std::string>>{
             {"Jimi Hendrix", "The Jimi Hendrix Experience"},
             {"Alice Meadow", "Orchestra featuring Alice Meadow"},
             {"Zoë Rivera", "The Zoë Rivera Ensemble"},
             {"Echo Echo", "The Echo Echo Ensemble"}})
        for (bool reverse : {false, true}) {
            std::vector<rym::Track> local{track("Song", {reverse ? long_name : short_name})};
            rym::Index variants(local);
            auto request = song("Song", {reverse ? short_name : long_name});
            auto result = variants.suggest(request, {9, 10}, {85, 100});
            require(!variants.owns(request) && result.size() == 1 && result[0].artist_gap,
                    "Artist phrase overlap is symmetric and Unicode-safe");
            equal_candidates(result, variants.suggest(request, {9, 10}, {85, 100}, true));
        }
    for (const auto &[short_name, long_name] : std::vector<std::pair<std::string, std::string>>{
             {"Hendrix", "The Jimi Hendrix Experience"},
             {"The National", "National Symphony Orchestra"},
             {"Jimi Hendrix", "The Jimi Hendrixson Experience"},
             {"Jimi Hendrix", "Jimi Young and Tony Hendrix"},
             {"Jimi and", "The Jimi and Orchestra"}}) {
        std::vector<rym::Track> local{track("Song", {long_name})};
        rym::Index variants(local);
        auto request = song("Song", {short_name});
        auto result = variants.suggest(request, {9, 10}, {85, 100});
        require(result.empty(),
                "Single words, generic words, and partial words do not imply an artist variant");
        equal_candidates(result, variants.suggest(request, {9, 10}, {85, 100}, true));
    }
    std::vector<rym::Track> large;
    for (std::size_t i = 0; i < 60; ++i) {
        auto name = "Performer " + std::to_string(i);
        large.push_back(track("Song", {name}, std::to_string(i) + ".flac"));
        large.push_back(track("Song [Single Version]", {"The " + name + " Orchestra"},
                              std::to_string(i) + "-single.flac"));
    }
    rym::Index varied(large);
    for (const auto &local : large)
        for (auto cutoff : {rym::Fraction{0, 1}, rym::Fraction{9, 10}, rym::Fraction{1, 1}}) {
            auto request = song(local.title, local.artists);
            equal_candidates(varied.suggest(request, cutoff, {85, 100}),
                             varied.suggest(request, cutoff, {85, 100}, true));
        }
}
void slash_artist_credit_review() {
    for (auto credit : {"Clams Casino/ Imogen Heap", "Clams Casino / Imogen Heap",
                        "Clams Casino/Imogen Heap", "Imogen Heap/Clams Casino",
                        "Other Artist / Clams Casino / Imogen Heap"})
        for (bool reverse : {false, true}) {
            std::vector<rym::Track> local{
                track("I'm God", {reverse ? "Clams Casino" : credit})};
            rym::Index index(local);
            auto query = song("I'm God", {reverse ? credit : "Clams Casino"});
            auto result = index.suggest(query, {9, 10}, {85, 100});
            require(!index.owns(query) && result.size() == 1 && result[0].artist_gap &&
                        !result[0].credit_gap && !rym::passes(result[0].artist, {9, 10}) &&
                        result[0].title.numerator == result[0].title.denominator,
                    "Slash-delimited multiword credit overlap needs review with honest scores");
            equal_candidates(result, index.suggest(query, {9, 10}, {85, 100}, true));
            require(index.suggest(song("I'm Gods", query.artists), {9, 10}, {85, 100}).empty(),
                    "Slash credit overlap still requires an exact base title");
        }
    std::vector<rym::Track> local{track("I'm God", {"Clams Casino/ Imogen Heap"})};
    rym::Index index(local);
    require(index.owns(song("I'm God", {"Clams Casino/ Imogen Heap"})),
            "An unchanged complete slash credit can establish exact ownership");
    require(!rym::Whitelist::parse("songs=[{title=\"I'm God\",artist='Clams Casino/ Imogen Heap'}]")
                 .contains(song("I'm God", {"Clams Casino"})),
            "Slash review boundaries do not broaden whitelist identities");
    rym::Scan scan;
    scan.tracks = local;
    auto report = rym::parse_report(rym::report_json(rym::compare_library(
        {{song("I'm God", {"Clams Casino"})}, rym::now_seconds()}, scan, ".", false,
        {9, 10}, {85, 100})));
    require(!report.songs[0].owned && report.songs[0].candidates.size() == 1 &&
                report.songs[0].candidates[0].artist_gap,
            "Slash-credit review survives comparison and report serialization");
    for (const auto &[short_name, full_credit] :
         std::vector<std::pair<std::string, std::string>>{
             {"AC", "AC/DC"}, {"DC", "AC/DC"}, {"AC/DC", "AC/DC / Guest Artist"},
             {"Clams", "Clams/ Imogen Heap"},
             {"Alpha / Beta", "Alpha / Beta / Guest Artist"},
             {"The / National", "The / National / Guest Artist"},
             {"Clams Casino", "Clams Casinova/ Imogen Heap"},
             {"Clams Casino", "Other Clams/ Casino Artist"}}) {
        std::vector<rym::Track> tracks{track("Song", {full_credit})};
        rym::Index variants(tracks);
        auto query = song("Song", {short_name});
        auto result = variants.suggest(query, {9, 10}, {85, 100});
        require(result.empty(), "Slash review does not infer single-word or partial artists");
        equal_candidates(result, variants.suggest(query, {9, 10}, {85, 100}, true));
    }
    std::vector<rym::Track> varied;
    for (std::size_t i = 0; i < 60; ++i) {
        auto name = "Performer " + std::to_string(i);
        varied.push_back(track("Song", {name}, std::to_string(i) + ".flac"));
        varied.push_back(track("Song", {"Guest Artist/ " + name},
                               std::to_string(i) + "-guest.flac"));
    }
    rym::Index variants(varied);
    for (const auto &value : varied)
        for (auto cutoff : {rym::Fraction{0, 1}, rym::Fraction{9, 10}, rym::Fraction{1, 1}}) {
            auto query = song(value.title, value.artists);
            equal_candidates(variants.suggest(query, cutoff, {85, 100}),
                             variants.suggest(query, cutoff, {85, 100}, true));
        }
}
void diacritic_review() {
    for (const auto &[original, expected] :
         std::vector<std::pair<std::string, std::u32string>>{
             {"Bôa", U"boa"}, {"Été", U"ete"}, {"Ångström", U"angstrom"},
             {"façade", U"facade"}, {"Ắ", U"a"}, {"a\xCC\xA3\xCC\x81", U"a"}}) {
        auto result = rym::latin_diacritics(rym::normalize(original).points);
        require(result && *result == expected, "Latin diacritics have a separate review spelling");
    }
    for (auto original : {"BoA", "AC/DC", "が", "क़", "ά", "й", "未知", "ø", "a️", "a्"})
        require(!rym::latin_diacritics(rym::normalize(original).points),
                "ASCII, other scripts, variation selectors, and undecomposable letters stay intact");
    for (bool reverse : {false, true}) {
        std::vector<rym::Track> tracks{track("Duvet", {reverse ? "Bôa" : "BoA"})};
        auto query = song("Duvet", {reverse ? "BoA" : "Bôa"});
        rym::Index index(tracks);
        auto result = index.suggest(query, {9, 10}, {85, 100});
        require(!index.owns(query) && result.size() == 1 && result[0].diacritic_gap &&
                    !result[0].artist_gap && !result[0].credit_gap &&
                    result[0].artist.numerator == 2 && result[0].artist.denominator == 3,
                "Duvet by Bôa versus BoA needs review and retains its actual artist score");
        equal_candidates(result, index.suggest(query, {9, 10}, {85, 100}, true));
        require(index.owns(song("Duvet", tracks[0].artists)),
                "Unchanged accents still permit exact ownership");
        auto whitelist = rym::Whitelist(std::vector<rym::WhitelistEntry>{
            {{}, {}, "Duvet", tracks[0].artists}});
        require(!whitelist.contains(query), "Accent folding cannot broaden whitelist identity");
        rym::Scan scan{tracks, {}, 0};
        auto report = rym::parse_report(rym::report_json(rym::compare_library(
            {{query}, rym::now_seconds()}, scan, ".", false, {9, 10}, {85, 100})));
        require(!report.songs[0].owned && report.songs[0].candidates.size() == 1 &&
                    report.songs[0].candidates[0].diacritic_gap,
                "Diacritic review remains visible after report serialization");
    }
    std::vector<rym::Track> collaborations{track("Été", {"Á", "B"})};
    rym::Index collaboration(collaborations);
    auto request = song("Ete", {"B", "A"});
    auto result = collaboration.suggest(request, {1, 1}, {1, 1});
    require(!collaboration.owns(request) && result.size() == 1 && result[0].diacritic_gap &&
                result[0].title.numerator == 1 && result[0].title.denominator == 3,
            "Folded collaboration credits reorder safely and titles retain actual scores");
    equal_candidates(result, collaboration.suggest(request, {1, 1}, {1, 1}, true));
    for (const auto &[title, artists] :
         std::vector<std::pair<std::string, std::vector<std::string>>>{
             {"Duvet", {"Unrelated Artist"}}, {"Duvet (Live)", {"BoA"}},
             {"Duvet", {"BoA", "Bôa"}}}) {
        std::vector<rym::Track> tracks{track(title, artists)};
        rym::Index index(tracks);
        auto query = song("Duvet", {"Bôa"});
        auto candidates = index.suggest(query, {9, 10}, {85, 100});
        if (artists.size() == 1 && title == "Duvet (Live)")
            require(candidates.size() == 1 && candidates[0].version_gap &&
                        candidates[0].annotation_gap && candidates[0].diacritic_gap,
                    "Artist spelling review composes with existing exact-base recording review");
        else
            require(candidates.empty(),
                    "Accent review requires matching complete credits, preserving collisions");
        equal_candidates(candidates, index.suggest(query, {9, 10}, {85, 100}, true));
    }
    std::vector<rym::Track> long_names{
        track("é" + std::string(12000, 'x') + "é", {"BoA"})};
    auto long_query = song("e" + std::string(12000, 'x') + "e", {"Bôa"});
    auto long_result = rym::Index(long_names).suggest(long_query, {9, 10}, {85, 100});
    require(long_result.size() == 1 && long_result[0].diacritic_gap &&
                long_result[0].title.numerator == 12000 &&
                long_result[0].title.denominator == 12002,
            "Long accent reviews use a valid narrow edit bound for exact raw scores");
    std::vector<rym::Track> varied;
    for (int i = 0; i < 120; ++i)
        varied.push_back(track((i % 2 ? "Song " : "Sóng ") + std::to_string(i % 31),
                               {(i % 3 ? "Band " : "Bánd ") + std::to_string(i % 7)},
                               std::to_string(i) + ".flac"));
    rym::Index varied_index(varied);
    for (int i = 0; i < 80; ++i) {
        auto query = song((i % 2 ? "Song " : "Sòng ") + std::to_string(i % 31),
                          {(i % 3 ? "Band " : "Bànd ") + std::to_string(i % 7)});
        for (auto cutoff : {rym::Fraction{0, 1}, {9, 10}, {1, 1}})
            equal_candidates(varied_index.suggest(query, cutoff, cutoff),
                             varied_index.suggest(query, cutoff, cutoff, true));
    }
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
void combined_review_oracle() {
    const std::vector<std::string> forms{
        "Melody Part 3", "Melody Pt.3 feat. Guest", "Melody Pt.03", "Melody Part 03",
        "Mélody Pt.3 [Notes]", "Melody (feat. Guest)", "Melody (Live)",
        "Melody Pt.3 (Acoustic)", "Intro/Melody Pt.3/Finale", "Melody Part 3 - Outro",
        "Melody [Nested (notes)] Pt.3", "Melody [Broken Pt.3", "Melody Vol.2 ft. Guest",
        "Melody Volume 2", "Melody No.3", "Melody Number 3", "Melody Pt.III"};
    const std::vector<std::vector<std::string>> credits{
        {"Band"}, {"Bánd"}, {"Band", "Guest"}, {"Guest", "Band"},
        {"Band | Guest"}, {"Band", "Guést"}, {"The Band Experience"},
        {"Band/ Guest"}, {"Other Artist"}};
    std::mt19937 random(6317);
    std::vector<rym::Track> tracks;
    for (std::size_t i = 0; i < 150; ++i)
        tracks.push_back(track(forms[random() % forms.size()], credits[random() % credits.size()],
                               std::to_string(i) + ".flac"));
    for (int order = 0; order < 2; ++order) {
        if (order)
            std::ranges::reverse(tracks);
        rym::Index index(tracks);
        for (std::size_t i = 0; i < 60; ++i) {
            auto query = song(forms[i % forms.size()], credits[i % credits.size()]);
            for (auto cutoff : {rym::Fraction{0, 1}, rym::Fraction{85, 100}, rym::Fraction{1, 1}})
                equal_candidates(index.suggest(query, cutoff, cutoff),
                                 index.suggest(query, cutoff, cutoff, true));
        }
    }
    for (auto prefix : {std::string("Melody"), std::string(500, 'x') + " melody"}) {
        std::vector<rym::Track> dense;
        for (std::size_t i = 0; i < 45; ++i) {
            auto title = prefix + (i % 3 ? " Pt.3 feat. " : " Part 3 featuring ") +
                         std::string(i + 1, 'g');
            if (i % 5 == 0)
                title += " (Live)";
            auto artists = i % 2 ? std::vector<std::string>{"Band", "Guest"}
                                 : std::vector<std::string>{"Band"};
            dense.push_back(track(title, artists, std::to_string(i) + ".flac"));
        }
        std::vector<std::filesystem::path> expected_paths;
        for (int order = 0; order < 2; ++order) {
            if (order)
                std::ranges::reverse(dense);
            rym::Index index(dense);
            auto query = song(prefix + " Part 3", {"Band"});
            auto best = index.suggest(query, {9, 10}, {1, 1});
            require(best.size() == 3, "Dense notation matches retain three suggestions");
            equal_candidates(best, index.suggest(query, {9, 10}, {1, 1}, true));
            std::vector<std::filesystem::path> paths;
            for (const auto &candidate : best)
                paths.push_back(dense[candidate.track_id].path);
            if (order)
                require(paths == expected_paths, "Notation distance bounds preserve deterministic ties");
            else
                expected_paths = std::move(paths);
            equal_candidates(index.suggest(query, {9, 10}, {85, 100}),
                             index.suggest(query, {9, 10}, {85, 100}, true));
        }
    }
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
            for (auto limit : {std::numeric_limits<std::size_t>::max() - 1,
                               std::numeric_limits<std::size_t>::max()})
                require(rym::edit_distance(a, b, limit) == expected,
                        "Unbounded edit budgets cannot overflow the band or failure sentinel");
            for (std::size_t limit = 0; limit <= std::max(a.size(), b.size()); ++limit) {
                auto actual = rym::edit_distance(a, b, limit);
                require(expected <= limit ? actual == expected : actual > limit,
                        "Bounded distance differs from independent DP");
            }
        }
    std::mt19937 random(8193);
    for (int trial = 0; trial < 120; ++trial) {
        std::u32string a(12 + random() % 180, U'a');
        for (auto &point : a)
            point = U"abéλ"[random() % 4];
        auto b = a;
        for (std::size_t i = 0; i < 1 + random() % 24; ++i)
            b[random() % b.size()] = U"abéλ"[random() % 4];
        if (trial % 2)
            b.insert(random() % b.size(), 1, U'x');
        if (trial % 3)
            b.erase(random() % b.size(), 1);
        auto expected = reference(a, b);
        for (auto limit : {std::size_t{0}, expected ? expected - 1 : 0, expected, expected + 1,
                           std::max(a.size(), b.size())}) {
            auto actual = rym::edit_distance(a, b, limit);
            auto reversed = rym::edit_distance(b, a, limit);
            require(expected <= limit ? actual == expected && reversed == expected
                                      : actual > limit && reversed > limit,
                    "Changing band boundaries cannot read stale worker scratch values");
        }
    }
    std::u32string long_name(4096, U'a'), changed = long_name;
    for (std::size_t i = 10; i < changed.size(); i += 200)
        changed[i] = U'b';
    require(rym::edit_distance(long_name, changed, 21) == 21 &&
                rym::edit_distance(changed, long_name, 20) > 20,
            "Long narrow bands retain exact edit-distance boundaries");
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
    require(rym::parse_album("<script type='application/ld+json'>{\"inAlbum\":{\"name\":\"Unknown\"}}</script>") == "Unknown",
            "A real album named Unknown remains valid metadata");
    for (auto name : {"", " ", "\\u00a0", "\\u0000", "\\ud800", "\\t\\n"}) {
        auto html = std::string("<script type='application/ld+json'>{\"inAlbum\":{\"name\":\"") +
                    name + "\"}}</script>";
        require(rym::parse_album(html).empty(), "Invalid or blank album metadata remains unavailable");
    }
    require(rym::parse_album("<div class='song_album'>First</div><div class='song_album'>Second</div>").empty(),
            "Ambiguous album metadata cannot select an arbitrary release");
    throws([] { rym::parse_album("<title>Just a moment...</title>"); });
    for (auto field : {"Alpha &amp; Omega", "Example Artist"}) {
        auto invalid = rym::read_file(fixture / "chart.html");
        invalid.replace(invalid.find(field), std::string_view(field).size(), " ");
        throws([&] { rym::parse_chart(invalid, "https://rateyourmusic.com/charts/top/song/all-time/"); });
    }
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
    require(rym::browser_page_state("https://rateyourmusic.com/", second, "complete",
                                    "<title>RYM homepage</title>") == State::loading &&
                rym::browser_page_state("https://rateyourmusic.com/", second, "complete",
                                        "<title>RYM homepage</title>", true) == State::verification,
            "A homepage cannot satisfy chart retrieval and needs recovery only after the deadline");
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
    std::size_t homepage_attempts{}, homepage_prompts{};
    rym::AccessTransport homepage_redirect(
        [&](std::string_view url) {
            if (++homepage_attempts == 1) {
                require(rym::browser_page_state("https://rateyourmusic.com/", url, "complete",
                                                "<title>Homepage</title>", true) ==
                            rym::BrowserPageState::verification,
                        "An unexpected homepage requires recovery after the loading deadline");
                throw rym::VerificationRequired("Chart request returned the homepage");
            }
            return mock.fetch(url);
        },
        [&](std::string_view url, rym::AccessReason reason) {
            ++homepage_prompts;
            require(url == "https://rateyourmusic.com/charts/top/song/all-time/" &&
                        reason == rym::AccessReason::verification,
                    "Homepage recovery opens the requested chart rather than the redirected URL");
        });
    require(rym::fetch_chart(homepage_redirect, 2).songs.size() == 2 &&
                homepage_attempts == 2 && homepage_prompts == 1,
            "A homepage redirect retries the original chart after one access handoff");
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
void album_retries() {
    class Mock final : public rym::Transport {
        std::function<std::string(std::string_view)> fetch_;
      public:
        explicit Mock(std::function<std::string(std::string_view)> fetch) : fetch_(std::move(fetch)) {}
        std::string fetch(std::string_view url) override { return fetch_(url); }
    };
    auto prepare = [] {
        rym::Snapshot snapshot;
        for (std::size_t rank = 1; rank <= 3; ++rank) {
            auto item = song("Song " + std::to_string(rank), {"Band"});
            item.rank = rank;
            item.url = "https://rateyourmusic.com/song/band/song-" + std::to_string(rank) + "/";
            if (rank == 3)
                item.album = "Chart Album";
            snapshot.songs.push_back(std::move(item));
        }
        return snapshot;
    };
    auto missing_rows = [](const rym::Snapshot &snapshot) {
        std::vector<rym::Missing> missing;
        for (const auto &item : snapshot.songs)
            missing.push_back({item, {}});
        return missing;
    };
    auto album = [](std::string_view name) {
        return "<div class='song_album'>" + std::string(name) + "</div>";
    };
    auto snapshot = prepare();
    std::array<unsigned, 3> visits{};
    auto warnings = rym::enrich_albums(missing_rows(snapshot), snapshot, [&] {
        return std::make_unique<Mock>([&](std::string_view url) {
            auto id = url == snapshot.songs[0].url ? 0 : 1;
            auto attempt = ++visits[id];
            if (id == 0 && attempt == 1)
                throw rym::Error("Temporary navigation failure");
            return id == 0 && attempt == 2 ? std::string("<html>Loading</html>") : album("Recovered Album");
        });
    }, 1);
    require(warnings.empty() && visits == std::array<unsigned, 3>{3, 1, 0} &&
                snapshot.songs[0].album == "Recovered Album" && snapshot.songs[2].album == "Chart Album",
            "Transient album failures retry at most twice without refetching known albums");
    snapshot = prepare();
    visits = {};
    warnings = rym::enrich_albums(missing_rows(snapshot), snapshot, [&] {
        return std::make_unique<Mock>([&](std::string_view url) {
            auto id = url == snapshot.songs[0].url ? 0 : 1;
            ++visits[id];
            if (id == 0)
                throw rym::Error("Persistent navigation failure");
            return album("Unknown");
        });
    }, 1);
    require(visits == std::array<unsigned, 3>{3, 1, 0} && warnings.size() == 1 &&
                warnings[0].find("#1 Song 1") != warnings[0].npos &&
                warnings[0].find("Persistent navigation failure") != warnings[0].npos &&
                snapshot.songs[1].album == "Unknown",
            "An exhausted album job cannot cancel unrelated jobs or reject a real Unknown album");
    snapshot = prepare();
    visits = {};
    warnings = rym::enrich_albums(missing_rows(snapshot), snapshot, [&] {
        return std::make_unique<Mock>([&](std::string_view url) {
            auto id = url == snapshot.songs[0].url ? 0 : 1;
            ++visits[id];
            return std::string("<div class='song_album'>First</div><div class='song_album'>Second</div>");
        });
    }, 1);
    require(visits == std::array<unsigned, 3>{3, 3, 0} && warnings.size() == 2 &&
                snapshot.songs[0].album.empty() && snapshot.songs[1].album.empty(),
            "Persistent metadata ambiguity has a finite retry budget and preserves unavailable values");
    for (unsigned restriction = 0; restriction < 3; ++restriction) {
        snapshot = prepare();
        unsigned calls{};
        warnings = rym::enrich_albums(missing_rows(snapshot), snapshot, [&] {
            return std::make_unique<Mock>([&](std::string_view) -> std::string {
                ++calls;
                if (restriction == 0)
                    throw rym::VerificationRequired("Security check");
                if (restriction == 1)
                    throw rym::SignInRequired("Sign in");
                return "<title>Just a moment...</title>";
            });
        }, 1);
        require(calls == 1 && warnings.size() == 2 && snapshot.songs[0].album.empty(),
                "Access restrictions stop retries and queued album jobs immediately");
    }
    snapshot = prepare();
    warnings = rym::enrich_albums(missing_rows(snapshot), snapshot,
        []() -> std::unique_ptr<rym::Transport> { throw rym::Error("Worker initialization failure"); }, 1);
    require(warnings.size() == 2, "Worker initialization failure produces saved metadata warnings");
    unsigned calls{};
    throws([&] {
        rym::enrich_albums(missing_rows(snapshot), snapshot, [&] {
            return std::make_unique<Mock>([&](std::string_view) -> std::string {
                ++calls;
                rym::cancelled.store(true);
                throw rym::Error("Cancelled during navigation");
            });
        }, 1);
    });
    rym::cancelled.store(false);
    require(calls == 1, "Cancellation cannot retry album requests");
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
             {"matching metadata examples", matching_examples},
             {"shared matching pipeline", matching_pipeline},
             {"edition review", edition_review},
             {"general annotation review", general_annotation_review},
             {"matching composition and ranking", matching_composition},
             {"included section review", included_section_review},
             {"compound title review", compound_title_review},
             {"dash title review", dash_title_review},
             {"title notation review", title_notation_review},
             {"artist group boundaries", artist_group_boundaries},
             {"normalized component review", normalized_component_review},
             {"artist credit overlap", artist_credit_overlap},
             {"slash artist credit review", slash_artist_credit_review},
             {"diacritic review", diacritic_review},
             {"matching", matching},
             {"indexed oracle", oracle},
             {"combined review oracle", combined_review_oracle},
             {"distance oracle", distance_oracle},
             {"HTML parsing", parsing},
             {"report review", report_review_tests},
             {"launcher", launcher_tests},
             {"chart acquisition", acquisition},
             {"browser reply ownership", browser_protocol},
             {"browser socket failures", browser_socket_tests},
             {"album workers", album_workers},
             {"album retries", album_retries},
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
