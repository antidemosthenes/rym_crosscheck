#pragma once
#include <boost/container/static_vector.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rym {
struct Song {
    std::size_t rank{};
    std::string title;
    std::vector<std::string> artists;
    std::string album;
    std::string url;
};
struct Track {
    std::filesystem::path path;
    std::string title;
    std::vector<std::string> artists;
    std::string album;
    std::uintmax_t size{};
    std::int64_t modified{};
};
struct Fraction {
    std::uint32_t numerator{1};
    std::uint32_t denominator{1};
};
struct Candidate {
    std::size_t track_id{};
    Fraction artist;
    Fraction title;
    bool credit_gap{};
};
struct Missing {
    Song song;
    boost::container::static_vector<Candidate, 3> candidates;
};
struct Scan {
    std::vector<Track> tracks;
    std::vector<std::string> warnings;
    std::size_t reused{};
};
struct Snapshot {
    std::vector<Song> songs;
    std::int64_t captured{};
    std::string source{"https://rateyourmusic.com/charts/top/song/all-time/"};
};
}
