#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mirobody { namespace client {

struct WebSocketOptions {
    std::string url;                    // ws:// or wss://
    std::string subprotocol;            // optional Sec-WebSocket-Protocol
    std::vector<std::string> headers;   // extra request headers, "Key: value"
    int connect_timeout_ms = 10000;
    bool insecure_skip_verify = false;  // wss:// only: skip cert/hostname check
};

//------------------------------------------------------------------------------

struct WebSocketCallbacks {
    std::function<void()> on_open;
    std::function<void(const void* payload, std::size_t length, bool is_binary)> on_message;
    std::function<void(int code, const char* reason)> on_close;
    std::function<void(const char* message)> on_error;
};

//------------------------------------------------------------------------------
// WebSocketClient
//------------------------------------------------------------------------------

// Single-connection WebSocket client wrapping libwebsockets. One service thread
// is owned per instance; user callbacks fire on that thread and must not block.
// Send methods are safe to call from any thread.
class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&)            = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // Starts the service thread and initiates the handshake. Returns false if
    // the URL fails to parse or a previous connect is still active.
    bool connect(WebSocketOptions opts, WebSocketCallbacks cb);

    bool send_text(const char* payload);
    bool send_binary(const void* payload, std::size_t length);

    // Initiates a clean close and joins the service thread. Safe to call more
    // than once. The destructor calls this automatically.
    void close();

    bool is_connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
}
