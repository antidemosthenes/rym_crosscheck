#pragma once
#include "rym_crosscheck/domain/models.hpp"
#include <string_view>
namespace rym {
struct Name {
    std::string text;
    std::u32string points;
};
struct Identity {
    Name title;
    Name credit;
    std::vector<std::string> artists;
    std::string principal;
    std::string version;
    std::string key;
};
Name normalize(std::string_view value, bool title = false);
Identity identity(std::string_view title, const std::vector<std::string> &artists);
bool review_versions(std::string_view left, std::string_view right) noexcept;
Fraction threshold(std::string_view value);
std::size_t edit_distance(std::u32string_view a, std::u32string_view b, std::size_t limit);
Fraction similarity(std::u32string_view a, std::u32string_view b, Fraction cutoff);
bool passes(Fraction score, Fraction cutoff) noexcept;
}
