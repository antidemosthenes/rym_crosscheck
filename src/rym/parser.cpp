#include "rym_crosscheck/rym/parser.hpp"
#include "rym_crosscheck/domain/runtime.hpp"
#include "rym_crosscheck/matching/normalize.hpp"
#include <algorithm>
#include <boost/json.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <charconv>
#include <functional>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/html/html.h>
#include <memory>
#include <regex>
#include <winhttp.h>

namespace rym {
namespace {
struct Destroy {
    void operator()(lxb_html_document_t *document) const noexcept {
        lxb_html_document_destroy(document);
    }
};
class Document {
    std::unique_ptr<lxb_html_document_t, Destroy> document_;

  public:
    explicit Document(std::string_view html) : document_(lxb_html_document_create()) {
        if (html.size() > 16 * 1024 * 1024)
            throw Error("HTML response exceeds 16 MiB.");
        if (!document_ || lxb_html_document_parse(document_.get(),
                                                  reinterpret_cast<const lxb_char_t *>(html.data()),
                                                  html.size()) != LXB_STATUS_OK)
            throw Error("Cannot parse HTML.");
    }
    lxb_dom_node_t *root() const {
        return lxb_dom_interface_node(document_.get());
    }
};
void visit(lxb_dom_node_t *root, const std::function<void(lxb_dom_node_t *)> &function) {
    auto *node = root;
    while (node) {
        function(node);
        if (node->first_child) {
            node = node->first_child;
            continue;
        }
        while (node != root && !node->next)
            node = node->parent;
        if (node == root)
            break;
        node = node->next;
    }
}
std::string attribute(lxb_dom_node_t *node, std::string_view name) {
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return {};
    std::size_t length{};
    auto *value = lxb_dom_element_get_attribute(lxb_dom_interface_element(node),
                                                reinterpret_cast<const lxb_char_t *>(name.data()),
                                                name.size(), &length);
    return value ? std::string(reinterpret_cast<const char *>(value), length) : std::string{};
}
bool has_class(lxb_dom_node_t *node, std::string_view name) {
    auto value = attribute(node, "class");
    std::size_t offset{};
    while (offset < value.size()) {
        auto begin = value.find_first_not_of(" \t\r\n", offset);
        if (begin == value.npos)
            break;
        auto end = value.find_first_of(" \t\r\n", begin);
        if (end == value.npos)
            end = value.size();
        if (std::string_view(value).substr(begin, end - begin) == name)
            return true;
        offset = end;
    }
    return false;
}
std::string text(lxb_dom_node_t *node) {
    if (!node)
        return {};
    std::size_t length{};
    auto *raw = lxb_dom_node_text_content(node, &length);
    if (!raw)
        return {};
    std::string value(reinterpret_cast<char *>(raw), length);
    lxb_dom_document_destroy_text(node->owner_document, raw);
    auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == value.npos)
        return {};
    value = value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
    static const std::regex spaces("[\\t\\r\\n ]+");
    return std::regex_replace(value, spaces, " ");
}
lxb_dom_node_t *find_class(lxb_dom_node_t *node, std::string_view name) {
    lxb_dom_node_t *result{};
    visit(node, [&](auto *child) {
        if (!result && has_class(child, name))
            result = child;
    });
    return result;
}
std::string locale_text(lxb_dom_node_t *node) {
    if (!node)
        return {};
    if (auto *original = find_class(node, "ui_name_locale_original"))
        return text(original);
    if (auto *locale = find_class(node, "ui_name_locale"))
        return text(locale);
    return text(node);
}
std::string album_text(std::string value) {
    if (value.find('\0') != value.npos)
        return {};
    try {
        if (normalize(value).text.empty())
            return {};
    } catch (const Error &) {
        return {};
    }
    return value;
}
std::size_t rank_of(lxb_dom_node_t *row) {
    std::string rank = attribute(row, "data-rank");
    for (auto *ancestor = row->parent; ancestor && rank.empty(); ancestor = ancestor->parent) {
        auto id = attribute(ancestor, "id");
        if (id.starts_with("pos") && id.size() > 3 &&
            std::ranges::all_of(std::string_view(id).substr(3),
                                [](char c) { return c >= '0' && c <= '9'; }))
            rank = id.substr(3);
    }
    for (auto name : {"page_charts_section_charts_item_number",
                      "page_charts_section_charts_item_chart_position",
                      "page_charts_section_charts_item_rank", "chart_position"})
        if (rank.empty())
            if (auto *node = find_class(row, name))
                rank = text(node);
    auto begin = rank.find_first_of("0123456789");
    if (begin == rank.npos)
        throw Error("Chart rank not found; RYM markup may have changed.");
    std::size_t value{};
    auto [end, error] = std::from_chars(rank.data() + begin, rank.data() + rank.size(), value);
    if (error != std::errc{} || !value)
        throw Error("Invalid chart rank.");
    return value;
}
}
std::string site_url(std::string_view value, std::string_view base) {
    if (value.empty() || value.find_first_of("\r\n\t\\") != value.npos)
        throw Error("Invalid RYM URL.");
    std::string input(value);
    if (input.starts_with("//"))
        input = "https:" + input;
    else if (input.starts_with('/'))
        input = "https://rateyourmusic.com" + input;
    else if (input.find("://") == input.npos) {
        std::string prefix(base);
        auto slash = prefix.rfind('/');
        input = prefix.substr(0, slash + 1) + input;
    }
    auto url = wide(input);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != 443 ||
        normalize(utf8({parts.lpszHostName, parts.dwHostNameLength})).text != "rateyourmusic.com")
        throw Error("Only HTTPS rateyourmusic.com URLs are accepted.");
    auto path = utf8({parts.lpszUrlPath, parts.dwUrlPathLength});
    if (path.find("..") != path.npos)
        throw Error("Invalid RYM URL path.");
    auto extra = utf8({parts.lpszExtraInfo, parts.dwExtraInfoLength});
    auto fragment = extra.find('#');
    if (fragment != extra.npos)
        extra.resize(fragment);
    return "https://rateyourmusic.com" + path + extra;
}
std::string song_url(std::string_view value) {
    auto url = site_url(value);
    auto query = url.find('?');
    if (query != url.npos)
        url.resize(query);
    constexpr std::string_view prefix = "https://rateyourmusic.com/song/";
    if (!url.starts_with(prefix) || url.size() <= prefix.size())
        throw Error("Expected a RYM song URL.");
    if (!url.ends_with('/'))
        url += '/';
    return url;
}
bool challenged(std::string_view html) noexcept {
    const bool rym_restriction = html.find("recaptcha") != html.npos &&
                                 (html.find("some requests from your computer") != html.npos ||
                                  html.find("loading pages too quickly") != html.npos ||
                                  html.find("RYM/Sonemic Staff") != html.npos);
    return rym_restriction || html.find("cf-chl-") != html.npos ||
           html.find("_cf_chl_opt") != html.npos ||
           html.find("<title>Security check required</title>") != html.npos ||
           html.find("<title>Just a moment") != html.npos ||
           html.find("<title>Attention Required") != html.npos;
}
Page parse_chart(std::string_view html, std::string_view source) {
    if (challenged(html))
        throw Error("RYM verification page received.");
    Document document(html);
    Page page;
    visit(document.root(), [&](auto *node) {
        if (has_class(node, "page_charts_section_charts_item")) {
            Song song;
            song.rank = rank_of(node);
            auto *title = find_class(node, "page_charts_section_charts_item_title");
            if (!title)
                throw Error("Chart song title missing.");
            song.title = locale_text(title);
            visit(title, [&](auto *child) {
                auto href = attribute(child, "href");
                if (song.url.empty() && href.find("/song/") != href.npos)
                    song.url = song_url(href);
            });
            auto *credits = find_class(node, "page_charts_section_charts_item_credited_text");
            if (credits)
                visit(credits, [&](auto *child) {
                    if (has_class(child, "artist") &&
                        attribute(child, "href").find("/artist/") != std::string::npos) {
                        auto artist = locale_text(child);
                        if (!artist.empty() &&
                            std::ranges::find(song.artists, artist) == song.artists.end())
                            song.artists.push_back(std::move(artist));
                    }
                });
            if (auto *album = find_class(node, "page_charts_section_charts_item_album"))
                song.album = locale_text(album);
            if (song.album.empty())
                visit(node, [&](auto *child) {
                    if (!song.album.empty())
                        return;
                    auto cover = attribute(child, "alt");
                    constexpr std::string_view suffix = ", Cover art";
                    if (!cover.ends_with(suffix))
                        return;
                    for (const auto &artist : song.artists) {
                        auto prefix = artist + " - ";
                        if (cover.starts_with(prefix)) {
                            song.album = cover.substr(prefix.size(),
                                                      cover.size() - prefix.size() - suffix.size());
                            break;
                        }
                    }
                });
            song.album = album_text(std::move(song.album));
            if (song.url.empty())
                throw Error("Chart song URL missing; RYM markup may have changed.");
            identity(song.title, song.artists);
            page.songs.push_back(std::move(song));
        }
        auto rel = attribute(node, "rel");
        auto label = attribute(node, "aria-label");
        if (rel == "next" || label == "Next page" || has_class(node, "navlinknext") ||
            has_class(node, "ui_pagination_next")) {
            auto href = attribute(node, "href");
            if (!href.empty())
                page.next = site_url(href, source);
        }
    });
    if (page.songs.empty())
        throw Error("No song chart rows found; RYM may be blocked or its markup has changed.");
    if (!page.next.empty() &&
        !page.next.starts_with("https://rateyourmusic.com/charts/top/song/all-time/"))
        throw Error("Pagination points outside the requested chart.");
    return page;
}
std::string parse_album(std::string_view html) {
    if (challenged(html))
        throw Error("RYM verification page received instead of album metadata.");
    Document document(html);
    boost::unordered_flat_set<std::string> albums;
    boost::unordered_flat_set<std::string> header_albums;
    visit(document.root(), [&](auto *node) {
        if (has_class(node, "page_song_header_info_rest"))
            visit(node, [&](auto *child) {
                if (!has_class(child, "album"))
                    return;
                auto href = attribute(child, "href");
                if (!href.starts_with("/release/") &&
                    !href.starts_with("https://rateyourmusic.com/release/"))
                    return;
                auto name = album_text(locale_text(child));
                if (!name.empty())
                    header_albums.insert(std::move(name));
            });
        if (has_class(node, "song_album") || has_class(node, "page_song_album")) {
            auto album = album_text(locale_text(node));
            if (!album.empty())
                albums.insert(std::move(album));
        }
        if (attribute(node, "type") == "application/ld+json") {
            try {
                auto json = boost::json::parse(text(node));
                std::function<void(const boost::json::value &)> inspect = [&](const auto &value) {
                    if (value.is_array())
                        for (const auto &item : value.as_array())
                            inspect(item);
                    if (value.is_object()) {
                        const auto &object = value.as_object();
                        if (auto *album = object.if_contains("inAlbum")) {
                            if (album->is_object())
                                if (auto *name = album->as_object().if_contains("name");
                                    name && name->is_string())
                                    if (auto name_text = album_text(std::string(name->as_string()));
                                        !name_text.empty())
                                        albums.insert(std::move(name_text));
                        }
                        if (auto *graph = object.if_contains("@graph"))
                            inspect(*graph);
                    }
                };
                inspect(json);
            } catch (const std::exception &) {
            }
        }
    });
    if (!header_albums.empty())
        return header_albums.size() == 1 ? *header_albums.begin() : std::string{};
    return albums.size() == 1 ? *albums.begin() : std::string{};
}
void validate_chart(std::vector<Song> &songs, std::size_t top) {
    if (!top)
        throw Error("Top N must be positive.");
    std::ranges::sort(songs, {}, &Song::rank);
    boost::unordered_flat_set<std::string> urls;
    if (songs.size() < top)
        throw Error("Chart incomplete: requested " + std::to_string(top) + " songs, received " +
                    std::to_string(songs.size()) + ".");
    for (std::size_t i = 0; i < top; ++i) {
        auto &song = songs[i];
        if (song.rank != i + 1)
            throw Error("Chart has missing or conflicting ranks.");
        song.url = song_url(song.url);
        identity(song.title, song.artists);
        if (!urls.insert(song.url).second)
            throw Error("Chart contains duplicate song identities.");
    }
}
}
