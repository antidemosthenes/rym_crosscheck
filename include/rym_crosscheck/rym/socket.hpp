#pragma once
#include "rym_crosscheck/domain/runtime.hpp"
#include <memory>

namespace rym {
class BrowserSocket {
    struct Data;
    std::unique_ptr<Data> data_;

  public:
    explicit BrowserSocket(std::wstring_view endpoint);
    ~BrowserSocket() noexcept;
    BrowserSocket(const BrowserSocket &) = delete;
    BrowserSocket &operator=(const BrowserSocket &) = delete;
    void send(std::string message, bool cleanup, std::chrono::steady_clock::time_point deadline);
    std::string receive(bool cleanup, std::chrono::steady_clock::time_point deadline);
};
}
