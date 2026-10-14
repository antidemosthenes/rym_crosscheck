#pragma once
#include "rym_crosscheck/domain/runtime.hpp"
#include <memory>
namespace rym {
class VerificationRequired : public Error {
    using Error::Error;
};
void verify_browser(const std::filesystem::path &profile, std::chrono::seconds timeout);
class BrowserProfile {
    std::filesystem::path path_;
    bool temporary_{};

  public:
    BrowserProfile(const std::filesystem::path &path, bool temporary);
    ~BrowserProfile() noexcept;
    BrowserProfile(const BrowserProfile &) = delete;
    BrowserProfile &operator=(const BrowserProfile &) = delete;
    const std::filesystem::path &path() const noexcept {
        return path_;
    }
};
class Transport {
  public:
    virtual ~Transport() noexcept = default;
    virtual std::string fetch(std::string_view url) = 0;
};
class Http final : public Transport {
    Pacer &pacer_;

  public:
    explicit Http(Pacer &pacer);
    std::string fetch(std::string_view url) override;
};
class Browser final : public Transport {
    struct Data;
    std::unique_ptr<Data> data_;

  public:
    Browser(const std::filesystem::path &profile, Pacer &pacer);
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
