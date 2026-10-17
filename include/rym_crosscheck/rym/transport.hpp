#pragma once
#include "rym_crosscheck/domain/runtime.hpp"
#include <boost/json/object.hpp>
#include <functional>
#include <memory>
#include <optional>
namespace rym {
class VerificationRequired : public Error {
    using Error::Error;
};
class SignInRequired : public Error {
    using Error::Error;
};
enum class AccessReason { verification, sign_in };
std::optional<boost::json::object> browser_response(std::string_view message, std::int64_t id);
void resolve_browser_access(const std::filesystem::path &profile, std::chrono::seconds timeout,
                            bool launcher, std::string_view url, AccessReason reason);
enum class BrowserPageState { loading, ready, verification, sign_in };
BrowserPageState browser_page_state(std::string_view current, std::string_view requested,
                                    std::string_view ready, std::string_view html,
                                    bool expired = false) noexcept;
class Transport {
  public:
    virtual ~Transport() noexcept = default;
    virtual std::string fetch(std::string_view url) = 0;
};
class AccessTransport final : public Transport {
    std::function<std::string(std::string_view)> fetch_;
    std::function<void(std::string_view, AccessReason)> recover_;

  public:
    AccessTransport(std::function<std::string(std::string_view)> fetch,
                    std::function<void(std::string_view, AccessReason)> recover);
    std::string fetch(std::string_view url) override;
};
class Browser final : public Transport {
    struct Data;
    std::unique_ptr<Data> data_;
    bool launcher_{};

  public:
    Browser(const std::filesystem::path &profile, Pacer &pacer, bool launcher = false);
    ~Browser() noexcept;
    Browser(const Browser &) = delete;
    Browser &operator=(const Browser &) = delete;
    Browser(Browser &&) noexcept;
    Browser &operator=(Browser &&) noexcept;
    std::unique_ptr<Transport> fork();
    std::string fetch(std::string_view url) override;

  private:
    explicit Browser(std::unique_ptr<Data> data) noexcept;
};
}
