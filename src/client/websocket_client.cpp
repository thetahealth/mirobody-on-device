#include "client/websocket_client.hpp"

#include <libwebsockets.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mirobody { namespace client {

namespace {

struct ParsedUrl {
    std::string host;
    std::string path = "/";
    int port = 0;
    bool tls = false;
    bool valid = false;
};

ParsedUrl parse_ws_url(const std::string& url) {
    ParsedUrl out;
    std::string rest;
    if (url.size() >= 6 && url.compare(0, 6, "wss://") == 0) {
        out.tls  = true;
        out.port = 443;
        rest     = url.substr(6);
    } else if (url.size() >= 5 && url.compare(0, 5, "ws://") == 0) {
        out.tls  = false;
        out.port = 80;
        rest     = url.substr(5);
    } else {
        return out;
    }

    const std::size_t slash = rest.find('/');
    const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    if (slash != std::string::npos) out.path = rest.substr(slash);

    const std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
        out.host = authority;
    } else {
        out.host = authority.substr(0, colon);
        try {
            out.port = std::stoi(authority.substr(colon + 1));
        } catch (...) {
            return out;
        }
    }

    out.valid = !out.host.empty() && out.port > 0;
    return out;
}

struct PendingFrame {
    std::vector<uint8_t> data;
    bool is_binary = false;

    // The default member initializer above makes this a non-aggregate under C++11,
    // so an explicit constructor is needed for tx_queue.push_back({...}) to compile
    // (MSVC and C++14+ treat it as an aggregate and accept brace-init without this).
    PendingFrame() = default;
    PendingFrame(std::vector<uint8_t> d, bool bin) : data(std::move(d)), is_binary(bin) {}
};

}

//------------------------------------------------------------------------------

struct WebSocketClient::Impl {
    WebSocketOptions  opts;
    WebSocketCallbacks cb;
    ParsedUrl         url;

    lws_context* context = nullptr;
    lws*         wsi     = nullptr;

    std::thread       thread;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> should_close{false};

    std::mutex             tx_mtx;
    std::deque<PendingFrame> tx_queue;

    // Reassembly buffer for inbound messages. lws delivers a WebSocket message
    // in one or more fragments (when it exceeds the rx buffer); we accumulate
    // them and surface on_message once per complete message, so callers never
    // see a partial frame (e.g. half a JSON object). Touched only on the service
    // thread, so it needs no lock.
    std::string rx_buf;

    static int callback(lws* wsi, lws_callback_reasons reason,
                        void* user, void* in, size_t len);

    void run();
    void enqueue(std::vector<uint8_t> data, bool binary);
    void fire_error(const char* msg);
};

//------------------------------------------------------------------------------

int WebSocketClient::Impl::callback(lws* wsi, lws_callback_reasons reason,
                                    void* user, void* in, size_t len) {
    (void)user;
    Impl* self = nullptr;
    if (wsi) {
        auto* ctx = lws_get_context(wsi);
        if (ctx) {
            self = static_cast<Impl*>(lws_context_user(ctx));
        }
    }
    if (!self) return 0;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
        auto** p   = reinterpret_cast<unsigned char**>(in);
        auto*  end = *p + len;
        for (const auto& h : self->opts.headers) {
            auto colon = h.find(':');
            if (colon == std::string::npos) continue;
            std::string name = h.substr(0, colon);
            name.push_back(':');
            std::string val = h.substr(colon + 1);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
            if (lws_add_http_header_by_name(
                    wsi,
                    reinterpret_cast<const unsigned char*>(name.c_str()),
                    reinterpret_cast<const unsigned char*>(val.c_str()),
                    static_cast<int>(val.size()),
                    p, end) != 0) {
                return -1;
            }
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        self->connected.store(true);
        if (self->cb.on_open) self->cb.on_open();
        // Drain anything queued before the handshake finished.
        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
        self->rx_buf.append(static_cast<const char*>(in), len);
        // Deliver only once the whole message has arrived: the last fragment AND
        // nothing more buffered inside the current fragment.
        if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
            bool is_binary = lws_frame_is_binary(wsi) != 0;
            if (self->cb.on_message) {
                self->cb.on_message(self->rx_buf.data(), self->rx_buf.size(), is_binary);
            }
            self->rx_buf.clear();
        }
        break;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        std::lock_guard<std::mutex> lk(self->tx_mtx);
        if (self->tx_queue.empty()) break;
        auto frame = std::move(self->tx_queue.front());
        self->tx_queue.pop_front();
        std::vector<uint8_t> buf(LWS_PRE + frame.data.size());
        std::memcpy(buf.data() + LWS_PRE, frame.data.data(), frame.data.size());
        int n = lws_write(wsi, buf.data() + LWS_PRE, frame.data.size(),
                          frame.is_binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT);
        if (n < static_cast<int>(frame.data.size())) return -1;
        if (!self->tx_queue.empty()) lws_callback_on_writable(wsi);
        break;
    }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
        const char* msg = in ? static_cast<const char*>(in) : "connection error";
        self->fire_error(msg);
        self->wsi = nullptr;
        return -1;
    }

    case LWS_CALLBACK_CLIENT_CLOSED:
        self->connected.store(false);
        if (self->cb.on_close) self->cb.on_close(0, "");
        self->wsi = nullptr;
        break;

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
        // Cross-thread wakeup from enqueue() / close(). Re-arm WRITABLE.
        if (self->wsi) lws_callback_on_writable(self->wsi);
        break;

    default:
        break;
    }
    return 0;
}

//------------------------------------------------------------------------------

void WebSocketClient::Impl::run() {
    static lws_protocols protocols[] = {
        {"mirobody-ws-client", &Impl::callback, 0, 65536, 0, nullptr, 0},
        LWS_PROTOCOL_LIST_TERM
    };

    lws_context_creation_info info{};
    info.port      = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.user      = this;
    info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.timeout_secs = (opts.connect_timeout_ms + 999) / 1000;

    context = lws_create_context(&info);
    if (!context) {
        fire_error("lws_create_context failed");
        return;
    }

    lws_client_connect_info cci{};
    cci.context  = context;
    cci.address  = url.host.c_str();
    cci.port     = url.port;
    cci.path     = url.path.c_str();
    cci.host     = url.host.c_str();
    cci.origin   = url.host.c_str();
    cci.protocol = opts.subprotocol.empty() ? nullptr : opts.subprotocol.c_str();
    if (url.tls) {
        cci.ssl_connection = LCCSCF_USE_SSL;
        if (opts.insecure_skip_verify) {
            cci.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED
                                | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK
                                | LCCSCF_ALLOW_EXPIRED;
        }
    }

    wsi = lws_client_connect_via_info(&cci);
    if (!wsi) {
        fire_error("lws_client_connect_via_info failed");
        lws_context_destroy(context);
        context = nullptr;
        return;
    }

    while (!should_close.load()) {
        lws_service(context, 100);
    }

    if (wsi) {
        lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
        // Pump until the close handshake completes (bounded).
        for (int i = 0; i < 20 && connected.load(); ++i) {
            lws_service(context, 50);
        }
    }

    lws_context_destroy(context);
    context = nullptr;
}

//------------------------------------------------------------------------------

void WebSocketClient::Impl::enqueue(std::vector<uint8_t> data, bool binary) {
    {
        std::lock_guard<std::mutex> lk(tx_mtx);
        tx_queue.push_back({std::move(data), binary});
    }
    if (context) lws_cancel_service(context);
}

//------------------------------------------------------------------------------

void WebSocketClient::Impl::fire_error(const char* msg) {
    if (cb.on_error) cb.on_error(msg);
}

//------------------------------------------------------------------------------

WebSocketClient::WebSocketClient() : impl_(std::unique_ptr<Impl>(new Impl())) {}

WebSocketClient::~WebSocketClient() { close(); }

//------------------------------------------------------------------------------

bool WebSocketClient::connect(WebSocketOptions opts, WebSocketCallbacks cb) {
    if (impl_->running.load()) return false;
    impl_->opts = std::move(opts);
    impl_->cb   = std::move(cb);
    impl_->url  = parse_ws_url(impl_->opts.url);
    if (!impl_->url.valid) return false;

    impl_->should_close.store(false);
    impl_->connected.store(false);
    impl_->running.store(true);
    // Hoist the raw pointer into a local and capture it by value; an init-capture
    // ([impl = impl_.get()]) would be a C++14 feature and this builds as C++11.
    Impl* impl = impl_.get();
    impl_->thread = std::thread([impl] {
        impl->run();
        impl->running.store(false);
    });
    return true;
}

//------------------------------------------------------------------------------

bool WebSocketClient::send_text(const char* payload) {
    if (!impl_->running.load()) return false;
    const std::size_t n = std::strlen(payload);
    std::vector<uint8_t> data(payload, payload + n);
    impl_->enqueue(std::move(data), /*binary=*/false);
    return true;
}

//------------------------------------------------------------------------------

bool WebSocketClient::send_binary(const void* payload, std::size_t length) {
    if (!impl_->running.load()) return false;
    const auto* p = static_cast<const uint8_t*>(payload);
    std::vector<uint8_t> buf(p, p + length);
    impl_->enqueue(std::move(buf), /*binary=*/true);
    return true;
}

//------------------------------------------------------------------------------

void WebSocketClient::close() {
    if (!impl_->running.load() && !impl_->thread.joinable()) return;
    impl_->should_close.store(true);
    if (impl_->context) lws_cancel_service(impl_->context);
    if (impl_->thread.joinable()) impl_->thread.join();
}

//------------------------------------------------------------------------------

bool WebSocketClient::is_connected() const {
    return impl_->connected.load();
}

}
}
