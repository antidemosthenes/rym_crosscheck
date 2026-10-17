#include "rym_crosscheck/cli/options.hpp"
#include <algorithm>
#include <sstream>
#include <toml++/toml.hpp>

namespace rym {
ComparisonOptions default_options(const PortablePaths &paths) {
    ComparisonOptions value;
    value.cache = path_text(paths.cache);
    return value;
}
ComparisonOptions validate_options(ComparisonOptions value) {
    if (value.music.empty())
        throw Error("Choose a music folder.");
    auto absolute = [](std::string &text) {
        if (!text.empty())
            text = path_text(std::filesystem::absolute(wide(text)).lexically_normal());
    };
    absolute(value.music);
    absolute(value.cache);
    absolute(value.profile);
    std::error_code error;
    if (!std::filesystem::is_directory(wide(value.music), error))
        throw Error("Music folder does not exist or is unreadable.");
    std::filesystem::directory_iterator readable(wide(value.music), error);
    if (error)
        throw Error("Music folder is unreadable.");
    if (!value.top || value.top > static_cast<std::size_t>(INT_MAX))
        throw Error("Top N must be between 1 and 2147483647.");
    if (!value.local_workers || value.local_workers > 64)
        throw Error("Local workers must be between 1 and 64.");
    if (!value.web_workers || value.web_workers > 8)
        throw Error("Web workers must be between 1 and 8.");
    if (value.delay < 0)
        throw Error("Request delay cannot be negative.");
    if (value.verification < 1 || value.verification > 3600)
        throw Error("Verification timeout must be between 1 and 3600 seconds.");
    threshold(value.artist);
    threshold(value.title);
    if (value.cache.empty())
        throw Error("Cache directory cannot be empty.");
    return value;
}
boost::json::object options_json(const ComparisonOptions &v) {
    return {{"music", v.music},
            {"cache", v.cache},
            {"profile", v.profile},
            {"artist", v.artist},
            {"title", v.title},
            {"top", v.top},
            {"local_workers", v.local_workers},
            {"web_workers", v.web_workers},
            {"delay", v.delay},
            {"verification", v.verification},
            {"offline", v.offline},
            {"clear", v.clear}};
}
ComparisonOptions parse_options(const boost::json::object &json, const PortablePaths &paths) {
    auto value = default_options(paths);
    auto text = [&](const char *key, std::string &target) {
        if (auto *v = json.if_contains(key))
            target = std::string(v->as_string());
    };
    auto number = [&](const char *key, auto &target) {
        if (auto *v = json.if_contains(key)) {
            auto n = v->to_number<std::int64_t>();
            if (n < 0 || n > INT_MAX)
                throw Error("Comparison option is out of range.");
            target = static_cast<std::remove_reference_t<decltype(target)>>(n);
        }
    };
    text("music", value.music);
    text("cache", value.cache);
    text("profile", value.profile);
    text("artist", value.artist);
    text("title", value.title);
    number("top", value.top);
    number("local_workers", value.local_workers);
    number("web_workers", value.web_workers);
    number("delay", value.delay);
    number("verification", value.verification);
    if (auto *v = json.if_contains("offline"))
        value.offline = v->as_bool();
    if (auto *v = json.if_contains("clear"))
        value.clear = v->as_bool();
    return value;
}
ComparisonOptions load_settings(const PortablePaths &paths) {
    auto value = default_options(paths);
    if (!std::filesystem::exists(paths.settings))
        return value;
    auto table = toml::parse(read_file(paths.settings));
    if (table["schema"].value<int>() != 1)
        throw Error("Unsupported settings version.");
    boost::json::object json;
    for (auto key : {"music", "cache", "profile", "artist", "title"})
        if (auto s = table[key].value<std::string>())
            json[key] = *s;
    for (auto key : {"top", "local_workers", "web_workers", "delay", "verification"})
        if (auto n = table[key].value<std::int64_t>())
            json[key] = *n;
    return parse_options(json, paths);
}
void save_settings(const PortablePaths &paths, const ComparisonOptions &value) {
    auto json = options_json(value);
    toml::table table;
    table.insert("schema", 1);
    for (auto key : {"music", "cache", "profile", "artist", "title"}) {
        if (std::string_view(key) == "cache" &&
            std::filesystem::path(wide(value.cache)).lexically_normal() == paths.cache)
            continue;
        table.insert(key, std::string(json.at(key).as_string()));
    }
    for (auto key : {"top", "local_workers", "web_workers", "delay", "verification"})
        table.insert(key, json.at(key).to_number<std::int64_t>());
    std::ostringstream out;
    out << table;
    atomic_write(paths.settings, out.str());
}
std::vector<std::string> comparison_arguments(const ComparisonOptions &v) {
    std::vector<std::string> args{"--music-dir",
                                  v.music,
                                  "--top",
                                  std::to_string(v.top),
                                  "--cache-dir",
                                  v.cache,
                                  "--local-workers",
                                  std::to_string(v.local_workers),
                                  "--web-workers",
                                  std::to_string(v.web_workers),
                                  "--request-delay-ms",
                                  std::to_string(v.delay),
                                  "--verification-timeout",
                                  std::to_string(v.verification),
                                  "--artist-threshold",
                                  v.artist,
                                  "--title-threshold",
                                  v.title,
                                  "--no-open"};
    if (!v.profile.empty()) {
        args.push_back("--browser-profile");
        args.push_back(v.profile);
    }
    if (v.offline)
        args.push_back("--offline");
    if (v.clear)
        args.push_back("--clear-cache");
    return args;
}
std::wstring windows_command(const std::filesystem::path &executable,
                             const std::vector<std::string> &arguments) {
    auto quote = [](std::wstring_view text) {
        std::wstring out = L"\"";
        std::size_t slashes{};
        for (auto c : text) {
            if (c == L'\\') {
                ++slashes;
                continue;
            }
            out.append(slashes * (c == L'"' ? 2 : 1), L'\\');
            slashes = 0;
            if (c == L'"')
                out += L'\\';
            out += c;
        }
        out.append(slashes * 2, L'\\');
        out += L'"';
        return out;
    };
    auto result = quote(executable.native());
    for (const auto &arg : arguments) {
        result += L' ';
        result += quote(wide(arg));
    }
    return result;
}
}
