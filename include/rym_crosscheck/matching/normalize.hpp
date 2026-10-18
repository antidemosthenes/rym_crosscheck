#pragma once
#include "rym_crosscheck/domain/models.hpp"
#include <optional>
#include <string_view>
namespace rym {
struct Name {
    std::string text;
    std::u32string points;
};
struct AnnotationText {
    std::string base;
    std::string source;
    bool annotated{};
};
struct Identity {
    Name title;
    Name credit;
    std::vector<std::string> artists;
    std::string principal;
    std::string version;
    std::string sections;
    std::string key;
    AnnotationText annotations;
};
Name normalize(std::string_view value, bool title = false);
std::u32string decode_utf8(std::string_view value);
// Returns a review spelling only when Latin diacritics change.
std::optional<std::u32string> latin_diacritics(std::u32string_view normalized);
AnnotationText extract_annotations(std::string_view normalized);
Identity identity(std::string_view title, const std::vector<std::string> &artists);
std::string identity_key(std::string_view title, const std::vector<std::string> &artists);
bool review_versions(std::string_view left, std::string_view right) noexcept;
Fraction threshold(std::string_view value);
std::size_t edit_distance(std::u32string_view a, std::u32string_view b, std::size_t limit);
Fraction similarity(std::u32string_view a, std::u32string_view b, Fraction cutoff);
bool passes(Fraction score, Fraction cutoff) noexcept;
}
