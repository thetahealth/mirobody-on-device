#include "chat/transport/ws.hpp"

#include "chat/dispatcher.hpp"
#include "chat/event/event.hpp"
#include "chat/packet.hpp"
#include "chat/transport/responder.hpp"

#include <rapidjson/document.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mirobody { namespace chat {

namespace {

// Value of `key` in a urlencoded query string ("a=1&token=xyz"), or "" if
// absent. JWTs and session ids are URL-safe, so no percent-decoding is applied.
std::string ws_query_param(const std::string& query, const char* key) {
    const std::size_t klen = std::strlen(key);
    std::size_t i = 0;
    while (i < query.size()) {
        const std::size_t amp = query.find('&', i);
        const std::size_t end = (amp == std::string::npos) ? query.size() : amp;
        const std::size_t eq  = query.find('=', i);
        if (eq != std::string::npos && eq < end &&
            (eq - i) == klen && query.compare(i, klen, key) == 0) {
            return query.substr(eq + 1, end - eq - 1);
        }
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    return std::string();
}

// Per-connection state shared between the lws service thread (on_open/on_message/
// on_close) and the detached worker thread that runs a realtime turn. `mu` guards
// every field; `alive` flips false in on_close so the worker's responder stops
// forwarding (and aborts the turn) once the socket is gone, and `busy` serializes
// turns so a second frame can't race a turn already in flight. The worker only
// ever touches `wsi` while holding `mu` and seeing `alive`, so the lws callback
// that frees the connection (which also takes `mu`, via on_close) can never run
// concurrently with a send on a dying socket.
struct LiveSession {
    std::mutex      mu;
    bool            alive  = true;
    bool            busy   = false;
    lws*            wsi    = nullptr;
    server::Router* router = nullptr;
};

// Send one frame to the client iff the socket is still alive. Takes `mu`.
void live_send(LiveSession& s, const std::string& frame) {
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.alive) s.router->send_ws(s.wsi, frame, false);
}

// WebSocket outbound channel: each event is a JSON text frame; finish() sends the
// terminal {"type":"end"} frame. Every send is guarded by the session mutex and
// the alive flag, so it never races the socket teardown.
class WsResponder : public Responder {
public:
    explicit WsResponder(std::shared_ptr<LiveSession> state) : state_(std::move(state)) {}

    bool send(const Event& e) override {
        const std::string frame = e.to_json();
        std::lock_guard<std::mutex> lk(state_->mu);
        if (!state_->alive) return false;   // socket gone -> abort the turn
        state_->router->send_ws(state_->wsi,
            frame, false);
        return true;
    }

    void finish() override {
        live_send(*state_, EndEvent().to_json());
    }

private:
    std::shared_ptr<LiveSession> state_;
};

}   // namespace

WsTransport::WsTransport(server::Router& router, Dispatcher& dispatcher, const jwt::Jwt& jwt)
    : router_(router), dispatcher_(dispatcher), jwt_(jwt) {}

void WsTransport::start() {
    // Captured by the factory the router stores; all outlive the running server
    // (lifetime contract in the class doc).
    server::Router* router_ptr = &router_;
    const jwt::Jwt* jwt        = &jwt_;
    Dispatcher*     dispatcher = &dispatcher_;

    router_.ws("/api/chat", [router_ptr, jwt, dispatcher](const server::Request& req) -> server::WsHandlerHooks {
        server::WsHandlerHooks hooks;

        // Authenticate the handshake: bearer JWT from the Authorization header or
        // an ?access_token=/?token= query param (browsers can't set WS headers).
        // verify() strips a leading "Bearer " itself.
        std::string token = req.authorization;
        if (token.empty()) token = ws_query_param(req.query, "access_token");
        if (token.empty()) token = ws_query_param(req.query, "token");
        if (!jwt->verify(token).ok()) {
            hooks.reject = true;   // router refuses the upgrade
            return hooks;
        }

        std::shared_ptr<LiveSession> state = std::make_shared<LiveSession>();
        state->router = router_ptr;

        hooks.on_open = [state](lws* wsi) {
            std::lock_guard<std::mutex> lk(state->mu);
            state->wsi = wsi;
        };

        hooks.on_message = [state, dispatcher](lws* /*wsi*/, const std::string& payload, bool /*binary*/) {
            // Parse the frame into a kOpLive packet (the frame object is its
            // params). A malformed frame is reported like an empty request.
            rapidjson::Document d;
            if (d.Parse(payload.data(), payload.size()).HasParseError() || !d.IsObject()) {
                live_send(*state, ErrorEvent("empty request: provide \"messages\" or \"question\"").to_json());
                return;
            }
            std::shared_ptr<Packet> pkt =
                std::make_shared<Packet>(kOpLive, std::move(d));

            const rapidjson::Value& p = pkt->params();
            const bool has_msgs = p.HasMember("messages") && p["messages"].IsArray() &&
                                  !p["messages"].Empty();
            const bool has_q    = !pkt->str("question").empty();
            if (!has_msgs && !has_q) {
                live_send(*state, ErrorEvent("empty request: provide \"messages\" or \"question\"").to_json());
                return;
            }

            // One turn at a time: reject a second frame while a turn runs.
            {
                std::lock_guard<std::mutex> lk(state->mu);
                if (!state->alive) return;
                if (state->busy) {
                    const std::string frame = ErrorEvent("a turn is already in progress").to_json();
                    state->router->send_ws(state->wsi,
                        frame, false);
                    return;
                }
                state->busy = true;
            }

            std::thread([state, dispatcher, pkt]() {
                WsResponder responder(state);
                // The live socket carries no per-request identity; live turns
                // don't use it (live_response ignores user/session/timezone).
                dispatcher->dispatch(*pkt, 0, responder);

                std::lock_guard<std::mutex> lk(state->mu);
                state->busy = false;
            }).detach();
        };

        hooks.on_close = [state](lws* /*wsi*/) {
            std::lock_guard<std::mutex> lk(state->mu);
            state->alive = false;
        };

        return hooks;
    });
}

}}
