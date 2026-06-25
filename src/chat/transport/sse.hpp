#pragma once

// SseTransport -- the HTTP Server-Sent Events transport for the chat service.
//
// Registers POST /api/chat. It parses the request body (JSON, urlencoded, or a
// multipart upload) into a chat::Packet with code kOpChat -- attaching any
// uploaded file bytes as Packet attachments (the dispatcher's preprocess stores
// them and rewrites params.files) -- then runs the turn on a worker thread,
// streaming the dispatcher's events out as SSE (`data: {json}\n\n`) via an
// SseResponder, closed by a terminal {"type":"end"}. The worker keeps the lws
// service loop unblocked.

#include "chat/transport/transport.hpp"
#include "jwt/jwt.hpp"
#include "server/router.hpp"

namespace mirobody { namespace chat {

class Dispatcher;
class Packet;

class SseTransport : public Transport {
public:
    // `router`/`dispatcher`/`jwt` are borrowed and must outlive the transport.
    SseTransport(server::Router& router, Dispatcher& dispatcher, const jwt::Jwt& jwt);

    const char* name() const override { return "sse"; }
    void        start() override;   // registers POST /api/chat

private:
    void handle(const server::Request& req, server::Response& res);

    // Parse the request body into `out` (a kOpChat packet: params + any file-byte
    // attachments). Returns false and writes a 400 to `res` when the body is
    // malformed.
    bool parse_body(const server::Request& req, Packet& out, server::Response& res);

    server::Router& router_;
    Dispatcher&     dispatcher_;
    const jwt::Jwt& jwt_;
};

}}
