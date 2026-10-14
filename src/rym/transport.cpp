#include "rym_crosscheck/rym/transport.hpp"
#include "rym_crosscheck/rym/parser.hpp"
#include <curl/curl.h>

namespace rym {
namespace {
struct Global {
    Global() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw Error("Cannot initialize HTTP.");
    }
    ~Global() noexcept {
        curl_global_cleanup();
    }
};
struct Response {
    std::string body;
    bool verification{};
    bool exceeded{};
};
std::size_t append(char *buffer, std::size_t size, std::size_t count, void *context) noexcept {
    auto &result = *static_cast<Response *>(context);
    auto bytes = size * count;
    if (bytes > 16 * 1024 * 1024 - result.body.size()) {
        result.exceeded = true;
        return 0;
    }
    try {
        result.body.append(buffer, bytes);
        return bytes;
    } catch (...) {
        return 0;
    }
}
std::size_t header(char *buffer, std::size_t size, std::size_t count, void *context) noexcept {
    std::string_view value(buffer, size * count);
    if (value.starts_with("cf-mitigated: challenge") ||
        value.starts_with("Cf-Mitigated: challenge"))
        static_cast<Response *>(context)->verification = true;
    return size * count;
}
int progress(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return cancelled.load() ? 1 : 0;
}
}
Http::Http(Pacer &pacer) : pacer_(pacer) {
    static Global global;
}
std::string Http::fetch(std::string_view url) {
    auto target = site_url(url);
    pacer_.wait();
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(),
                                                               &curl_easy_cleanup);
    if (!handle)
        throw Error("Cannot create HTTP request.");
    Response response;
    curl_easy_setopt(handle.get(), CURLOPT_URL, target.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "rym-crosscheck/1.0");
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, header);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &response);
    curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(handle.get(), CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
    auto result = curl_easy_perform(handle.get());
    check_cancelled();
    if (response.exceeded)
        throw Error("HTTP response exceeds 16 MiB.");
    if (result != CURLE_OK)
        throw Error("HTTP failed: " + std::string(curl_easy_strerror(result)));
    long status{};
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
    if (response.verification || challenged(response.body) || status == 403)
        throw Error("RYM blocked HTTP access; use --web-mode browser and complete verification.");
    if (status != 200)
        throw Error("RYM returned HTTP " + std::to_string(status));
    return std::move(response.body);
}
}
