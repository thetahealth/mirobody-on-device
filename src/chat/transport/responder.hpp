#pragma once

// Responder -- a transport's outbound channel for one request (chat interface
// tier).
//
// The dispatcher (chat::Dispatcher) produces a turn's output as a stream of
// chat::Events (translated from the domain's llm::Events); the Responder is how
// it gets those events back to the user without knowing the wire protocol. Each
// event serializes itself (Event::to_json / to_sse), so a Responder just writes.
// Each transport supplies its own implementation bound to the request's output
// channel:
//
//   - SseResponder  writes `data: {json}\n\n` to the HTTP StreamWriter
//   - WsResponder   sends each event as a JSON text frame on the socket
//   - MqttResponder publishes each event to the request's reply topic
//
// send() returns false when the client is gone, which aborts the in-flight turn.
// finish() emits the terminal end-of-turn marker (an EndEvent) and closes the
// turn; the dispatcher calls it exactly once, after the turn completes.

#include "chat/event/event.hpp"

namespace mirobody { namespace chat {

class Responder {
public:
    virtual ~Responder() = default;

    // Forward one event to the user. Returns false if the client has gone (the
    // dispatcher then stops the turn).
    virtual bool send(const Event& e) = 0;

    // Emit the terminal end-of-turn marker (EndEvent) and close the response.
    virtual void finish() = 0;
};

}}
