#pragma once

// WsTransport -- the WebSocket transport for the chat service.
//
// Registers GET /api/chat as a WebSocket endpoint. The handshake is JWT-guarded
// (bearer header or ?access_token=/?token= query, since browsers can't set WS
// headers). Each inbound JSON frame ({provider, system?, messages[]|question}) is
// parsed into a chat::Packet with code kOpLive and run as one realtime turn on a
// worker thread, the dispatcher's events streamed back as JSON text frames via a
// WsResponder, closed by a terminal {"type":"end"}. One turn at a time per
// connection. Coexists with the SSE transport on /api/chat (GET-upgrade vs POST).

#include "chat/transport/transport.hpp"
#include "jwt/jwt.hpp"
#include "server/router.hpp"

namespace mirobody { namespace chat {

class Dispatcher;

class WsTransport : public Transport {
public:
    // `router`/`dispatcher`/`jwt` are borrowed and must outlive the transport.
    WsTransport(server::Router& router, Dispatcher& dispatcher, const jwt::Jwt& jwt);

    const char* name() const override { return "ws"; }
    void        start() override;   // registers WebSocket GET /api/chat

private:
    server::Router&  router_;
    Dispatcher&      dispatcher_;
    const jwt::Jwt&  jwt_;
};

}}
