#pragma once
#include "rym_crosscheck/domain/models.hpp"
#include <string_view>
namespace rym {
struct Page {
    std::vector<Song> songs;
    std::string next;
};
std::string site_url(std::string_view value, std::string_view base = "https://rateyourmusic.com/");
std::string song_url(std::string_view value);
bool challenged(std::string_view html) noexcept;
Page parse_chart(std::string_view html, std::string_view source);
std::string parse_album(std::string_view html);
void validate_chart(std::vector<Song> &songs, std::size_t top);
}
