#include "rym_crosscheck/rym/socket.hpp"
#include <array>
#include <winhttp.h>

namespace rym {
namespace {
struct InternetClose {
    void operator()(void *handle) const noexcept {
        if (handle)
            WinHttpCloseHandle(handle);
    }
};
using Internet = std::unique_ptr<void, InternetClose>;
class AsyncSession {
    Handle unloaded_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Internet handle_;

  public:
    AsyncSession() {
        if (!unloaded_)
            throw Error("Cannot create browser session cleanup event.");
        handle_.reset(WinHttpOpen(L"rym-crosscheck/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr,
                                  nullptr, WINHTTP_FLAG_ASYNC));
        auto event = unloaded_.get();
        if (!handle_ ||
            !WinHttpSetOption(handle_.get(), WINHTTP_OPTION_UNLOAD_NOTIFY_EVENT, &event,
                              sizeof(event)) ||
            !WinHttpSetTimeouts(handle_.get(), 2000, 2000, 2000, 2000))
            throw Error("Cannot initialize browser session cleanup.");
    }
    ~AsyncSession() noexcept {
        handle_.reset();
        WaitForSingleObject(unloaded_.get(), INFINITE);
    }
    HINTERNET get() const noexcept {
        return handle_.get();
    }
};
AsyncSession &shared_session() {
    static AsyncSession session;
    return session;
}
struct Operation {
    Handle completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle closed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<DWORD> expected{}, error{}, bytes{};
    std::atomic<WINHTTP_WEB_SOCKET_BUFFER_TYPE> type{};
    std::array<char, 32768> buffer;
    std::string outgoing;
    Operation() {
        if (!completed || !closed)
            throw Error("Cannot create browser operation events.");
    }
    void begin(DWORD status) noexcept {
        ResetEvent(completed.get());
        error.store(0);
        bytes.store(0);
        expected.store(status);
    }
};
struct Context {
    std::shared_ptr<Operation> operation;
};
void CALLBACK completed(HINTERNET, DWORD_PTR context, DWORD status, void *information,
                        DWORD length) noexcept {
    auto *owner = reinterpret_cast<Context *>(context);
    if (!owner)
        return;
    auto operation = owner->operation;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        delete owner;
        SetEvent(operation->closed.get());
    } else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
        operation->error.store(information && length >= sizeof(WINHTTP_ASYNC_RESULT)
                                   ? static_cast<WINHTTP_ASYNC_RESULT *>(information)->dwError
                                   : ERROR_GEN_FAILURE);
        SetEvent(operation->completed.get());
    } else if (status == operation->expected.load()) {
        if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE && information &&
            length >= sizeof(WINHTTP_WEB_SOCKET_STATUS)) {
            const auto &result = *static_cast<WINHTTP_WEB_SOCKET_STATUS *>(information);
            operation->bytes.store(result.dwBytesTransferred);
            operation->type.store(result.eBufferType);
        }
        SetEvent(operation->completed.get());
    }
}
DWORD_PTR observe(HINTERNET handle, const std::shared_ptr<Operation> &operation) {
    auto context = std::make_unique<Context>(operation);
    auto value = reinterpret_cast<DWORD_PTR>(context.get());
    if (!WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE, &value, sizeof(value)) ||
        WinHttpSetStatusCallback(handle, completed,
                                 WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
                                     WINHTTP_CALLBACK_FLAG_HANDLES,
                                 0) == WINHTTP_INVALID_STATUS_CALLBACK)
        throw Error("Cannot observe browser operations.");
    context.release();
    return value;
}
void wait(Internet &handle, const std::shared_ptr<Operation> &operation, bool cleanup,
          std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        if (!cleanup && cancelled.load()) {
            handle.reset();
            check_cancelled();
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            handle.reset();
            throw Error("Browser response timed out.");
        }
        auto result = WaitForSingleObject(operation->completed.get(), 25);
        if (result == WAIT_OBJECT_0) {
            if (auto error = operation->error.load()) {
                handle.reset();
                throw Error("Browser operation failed (Windows error " + std::to_string(error) +
                            ").");
            }
            return;
        }
        if (result != WAIT_TIMEOUT) {
            handle.reset();
            throw Error("Cannot wait for the browser operation.");
        }
    }
}
void started(DWORD error) {
    if (error && error != ERROR_IO_PENDING)
        throw Error("Cannot start browser operation (Windows error " + std::to_string(error) +
                    ").");
}
}
struct BrowserSocket::Data {
    std::shared_ptr<Operation> operation = std::make_shared<Operation>();
    AsyncSession &session = shared_session();
    Internet connection, socket;
    explicit Data(std::wstring_view endpoint) {
        std::wstring url(endpoint);
        if (url.starts_with(L"ws://"))
            url.replace(0, 5, L"http://");
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = parts.dwUrlPathLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
            throw Error("Invalid local browser endpoint.");
        auto host = std::wstring(parts.lpszHostName, parts.dwHostNameLength);
        if ((host != L"127.0.0.1" && host != L"localhost") || parts.nScheme != INTERNET_SCHEME_HTTP)
            throw Error("Browser endpoint must be local.");
        connection.reset(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
        Internet request(
            WinHttpOpenRequest(connection.get(), L"GET",
                               std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength).c_str(),
                               nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
        if (!request ||
            !WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
            throw Error("Cannot create browser handshake.");
        auto handshake = std::make_shared<Operation>();
        auto context = observe(request.get(), handshake);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        handshake->begin(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
        if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, context))
            started(GetLastError());
        wait(request, handshake, false, deadline);
        handshake->begin(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
        if (!WinHttpReceiveResponse(request.get(), nullptr))
            started(GetLastError());
        wait(request, handshake, false, deadline);
        DWORD status{}, size = sizeof(status);
        if (!WinHttpQueryHeaders(
                request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) ||
            status != 101)
            throw Error("Edge rejected the WebSocket handshake.");
        auto socket_context = std::make_unique<Context>(operation);
        socket.reset(WinHttpWebSocketCompleteUpgrade(
            request.get(), reinterpret_cast<DWORD_PTR>(socket_context.get())));
        if (!socket)
            throw Error("Edge WebSocket upgrade failed.");
        socket_context.release();
    }
    ~Data() noexcept {
        socket.reset();
        WaitForSingleObject(operation->closed.get(), 2000);
    }
};
BrowserSocket::BrowserSocket(std::wstring_view endpoint)
    : data_(std::make_unique<Data>(endpoint)) {}
BrowserSocket::~BrowserSocket() noexcept = default;
void BrowserSocket::send(std::string message, bool cleanup,
                         std::chrono::steady_clock::time_point deadline) {
    if (!data_->socket)
        throw Error("Browser connection is closed.");
    auto &operation = *data_->operation;
    operation.outgoing = std::move(message);
    operation.begin(WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE);
    started(WinHttpWebSocketSend(data_->socket.get(), WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 operation.outgoing.data(),
                                 static_cast<DWORD>(operation.outgoing.size())));
    wait(data_->socket, data_->operation, cleanup, deadline);
}
std::string BrowserSocket::receive(bool cleanup, std::chrono::steady_clock::time_point deadline) {
    if (!data_->socket)
        throw Error("Browser connection is closed.");
    auto &operation = *data_->operation;
    std::string message;
    for (;;) {
        operation.begin(WINHTTP_CALLBACK_STATUS_READ_COMPLETE);
        started(WinHttpWebSocketReceive(data_->socket.get(), operation.buffer.data(),
                                        static_cast<DWORD>(operation.buffer.size()), nullptr,
                                        nullptr));
        wait(data_->socket, data_->operation, cleanup, deadline);
        auto type = operation.type.load();
        if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
            type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            throw Error("Browser closed or sent a non-text response.");
        auto bytes = operation.bytes.load();
        if (message.size() + bytes > 32 * 1024 * 1024)
            throw Error("Browser message exceeds 32 MiB.");
        message.append(operation.buffer.data(), bytes);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            return message;
    }
}
}
