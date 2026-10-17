#include "rym_crosscheck/rym/socket.hpp"
#include "rym_crosscheck/rym/transport.hpp"
#include <array>
#include <bcrypt.h>
#include <wincrypt.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace {
void require(bool value, std::string_view message) {
    if (!value)
        throw rym::Error(std::string(message));
}
struct Connection {
    SOCKET value{INVALID_SOCKET};
    ~Connection() noexcept {
        if (value != INVALID_SOCKET)
            closesocket(value);
    }
};
struct Network {
    Network() {
        WSADATA data{};
        require(!WSAStartup(MAKEWORD(2, 2), &data), "Initialize local transport fixture");
    }
    ~Network() noexcept {
        WSACleanup();
    }
};
class Server {
    Network network_;
    Connection listener_;
    std::jthread worker_;
    unsigned short port_{};
    bool ready(SOCKET socket, bool writing, std::stop_token stop) const {
        while (!stop.stop_requested()) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(socket, &set);
            timeval timeout{0, 25000};
            auto count =
                select(0, writing ? nullptr : &set, writing ? &set : nullptr, nullptr, &timeout);
            if (count != 0)
                return count > 0;
        }
        return false;
    }
    bool send(SOCKET socket, std::string_view text, std::stop_token stop) const {
        while (!text.empty() && ready(socket, true, stop)) {
            auto count = ::send(socket, text.data(),
                                static_cast<int>(std::min<std::size_t>(text.size(), 65536)), 0);
            if (count <= 0) {
                if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK)
                    continue;
                return false;
            }
            text.remove_prefix(static_cast<std::size_t>(count));
        }
        return text.empty();
    }
    std::string receive(SOCKET socket, std::stop_token stop) const {
        std::array<char, 8192> buffer;
        while (ready(socket, false, stop)) {
            auto count = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (count < 0 && WSAGetLastError() == WSAEWOULDBLOCK)
                continue;
            return count > 0 ? std::string(buffer.data(), static_cast<std::size_t>(count)) : "";
        }
        return {};
    }
    bool frame(SOCKET socket, std::string_view data, unsigned char opcode, bool final,
               std::stop_token stop) const {
        std::string header{static_cast<char>((final ? 128 : 0) | opcode)};
        if (data.size() < 126)
            header += static_cast<char>(data.size());
        else {
            header += static_cast<char>(126);
            header += static_cast<char>(data.size() >> 8);
            header += static_cast<char>(data.size());
        }
        return send(socket, header, stop) && send(socket, data, stop);
    }
    void serve(std::stop_token stop, std::string mode) noexcept {
        try {
            if (!ready(listener_.value, false, stop))
                return;
            Connection client{accept(listener_.value, nullptr, nullptr)};
            if (client.value == INVALID_SOCKET)
                return;
            u_long nonblocking = 1;
            ioctlsocket(client.value, FIONBIO, &nonblocking);
            std::string request;
            while (request.find("\r\n\r\n") == request.npos && request.size() < 16384) {
                auto part = receive(client.value, stop);
                if (part.empty())
                    return;
                request += part;
            }
            if (mode == "handshake") {
                send(client.value, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n", stop);
                return;
            }
            if (mode == "handshake-cancel") {
                while (!receive(client.value, stop).empty()) {
                }
                return;
            }
            auto offset = request.find("Sec-WebSocket-Key:");
            if (offset == request.npos)
                return;
            offset += std::string_view("Sec-WebSocket-Key:").size();
            while (request[offset] == ' ')
                ++offset;
            auto key = request.substr(offset, request.find("\r\n", offset) - offset) +
                       "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            std::array<UCHAR, 20> digest;
            if (BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(key.data()),
                           static_cast<ULONG>(key.size()), digest.data(),
                           static_cast<ULONG>(digest.size())) < 0)
                return;
            DWORD length{};
            constexpr auto encoding = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
            CryptBinaryToStringA(digest.data(), static_cast<DWORD>(digest.size()), encoding,
                                 nullptr, &length);
            std::string encoded(length, '\0');
            if (!CryptBinaryToStringA(digest.data(), static_cast<DWORD>(digest.size()), encoding,
                                      encoded.data(), &length))
                return;
            while (!encoded.empty() && encoded.back() == '\0')
                encoded.pop_back();
            if (!send(client.value,
                      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                      "Upgrade\r\nSec-WebSocket-Accept: " +
                          encoded + "\r\n\r\n",
                      stop) ||
                receive(client.value, stop).empty())
                return;
            if (mode == "fragment") {
                std::string text(50000, 'x');
                if (!frame(client.value, text, 1, false, stop) ||
                    !frame(client.value, text, 0, true, stop))
                    return;
            } else if (mode == "close") {
                frame(client.value, {}, 8, true, stop);
                return;
            } else if (mode == "binary") {
                frame(client.value, "binary", 2, true, stop);
            } else if (mode == "oversized") {
                std::string chunk(60000, 'x');
                std::size_t remaining = 33 * 1024 * 1024;
                bool first = true;
                while (remaining) {
                    auto size = std::min(remaining, chunk.size());
                    if (!frame(client.value, std::string_view(chunk).substr(0, size), first ? 1 : 0,
                               remaining == size, stop))
                        return;
                    remaining -= size;
                    first = false;
                }
            } else if (mode == "events") {
                while (!stop.stop_requested()) {
                    if (!frame(client.value, R"({"method":"Page.loadEventFired","params":{}})", 1,
                               true, stop))
                        return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            while (!receive(client.value, stop).empty()) {
            }
        } catch (...) {
        }
    }

  public:
    explicit Server(std::string mode) {
        listener_.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(
            listener_.value != INVALID_SOCKET &&
                !bind(listener_.value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) &&
                !listen(listener_.value, 1),
            "Listen for a local browser fixture");
        int size = sizeof(address);
        require(!getsockname(listener_.value, reinterpret_cast<sockaddr *>(&address), &size),
                "Read fixture port");
        port_ = ntohs(address.sin_port);
        worker_ = std::jthread(
            [this, mode = std::move(mode)](std::stop_token stop) { serve(stop, mode); });
    }
    ~Server() noexcept {
        worker_.request_stop();
        if (worker_.joinable())
            worker_.join();
    }
    std::wstring endpoint() const {
        return L"ws://127.0.0.1:" + std::to_wstring(port_) + L"/socket";
    }
};
}
void browser_socket_tests() {
    using Clock = std::chrono::steady_clock;
    auto exercise = [](std::string_view mode) {
        Server server{std::string(mode)};
        bool failed{};
        auto start = Clock::now();
        std::size_t events{};
        std::jthread early_cancel;
        if (mode == "handshake-cancel")
            early_cancel = std::jthread([] {
                std::this_thread::sleep_for(std::chrono::milliseconds(75));
                rym::cancelled.store(true);
            });
        try {
            rym::BrowserSocket socket(server.endpoint());
            auto deadline = Clock::now() + std::chrono::seconds(3);
            socket.send("test", false, deadline);
            std::jthread cancellation;
            if (mode == std::string_view("cancel"))
                cancellation = std::jthread([] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(75));
                    rym::cancelled.store(true);
                });
            if (mode == std::string_view("stall") || mode == std::string_view("events"))
                deadline = Clock::now() + std::chrono::milliseconds(200);
            for (;;) {
                auto message = socket.receive(false, deadline);
                if (mode != std::string_view("events")) {
                    require(message == std::string(100000, 'x'), "Text fragments reassemble");
                    break;
                }
                require(!rym::browser_response(message, 1), "Events are not command responses");
                ++events;
            }
        } catch (const rym::Error &failure) {
            failed = true;
            if (mode == std::string_view("stall") || mode == std::string_view("events"))
                require(std::string_view(failure.what()).find("timed out") !=
                            std::string_view::npos,
                        "A stalled command expires instead of waiting indefinitely");
            if (mode == "oversized")
                require(std::string_view(failure.what()).find("32 MiB") != std::string_view::npos,
                        "Oversized browser messages are rejected at the application bound");
        }
        if (early_cancel.joinable())
            early_cancel.join();
        if (mode == "cancel" || mode == "handshake-cancel")
            require(rym::cancelled.load(), "Cancellation interrupts the pending operation");
        rym::cancelled.store(false);
        require(failed == (mode != std::string_view("fragment")), "Expected transport outcome");
        require(Clock::now() - start < std::chrono::seconds(3),
                "Transport cancellation and deadlines remain bounded");
        if (mode == std::string_view("events"))
            require(events > 0, "Asynchronous events cannot restart the command deadline");
    };
    for (const auto *mode : {"fragment", "stall", "cancel", "events", "close", "handshake",
                             "handshake-cancel", "binary", "oversized"})
        exercise(mode);
    // Warm WinHTTP's lazily allocated asynchronous worker resources before measuring cleanup.
    for (int run = 0; run < 4; ++run) {
        exercise("fragment");
        exercise("stall");
        exercise("cancel");
    }
    DWORD before{}, after{};
#ifdef _DEBUG
    _CrtMemState allocation_before{}, allocation_after{}, difference{};
    _CrtMemCheckpoint(&allocation_before);
#endif
    require(GetProcessHandleCount(GetCurrentProcess(), &before), "Count browser transport handles");
    for (int run = 0; run < 4; ++run) {
        exercise("fragment");
        exercise("stall");
        exercise("cancel");
    }
    auto settled = Clock::now() + std::chrono::seconds(2);
    do {
        require(GetProcessHandleCount(GetCurrentProcess(), &after), "Count closed browser handles");
        if (after <= before + 2)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (Clock::now() < settled);
#ifdef _DEBUG
    _CrtMemCheckpoint(&allocation_after);
    require(!_CrtMemDifference(&difference, &allocation_before, &allocation_after),
            "Repeated browser operations retain CRT allocations");
#endif
    require(after <= before + 2, "Repeated browser operations retain handles: before " +
                                     std::to_string(before) + ", after " + std::to_string(after));
}
