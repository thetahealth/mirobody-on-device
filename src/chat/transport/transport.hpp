#pragma once

// Transport -- the transport interface (chat interface tier).
//
// A transport bridges one wire protocol to the chat service. Its whole job is
// the two boundary translations:
//
//   inbound:   bytes  -> chat::Packet     (parse the request)
//   outbound:  llm::Event -> bytes        (serialize each response event)
//
// It does NOT run the turn -- it hands the Packet to a chat::Dispatcher together
// with a chat::Responder bound to this request's output channel, and the
// dispatcher drives the domain (chat::Chat) and pushes Events back through the
// Responder. So a transport knows only its wire, Packet, and Event; it never
// touches Chat directly. The concrete transports:
//
//   - SseTransport   POST /api/chat  -> Server-Sent Events   (transport/sse.*)
//
// The interface fixes only the lifecycle: construct the transport with what it
// needs, then start() it. ChatService owns the transports and starts them all.
// (The C ABI's mirobody_chat_messages drives the same dispatcher with no
// transport at all.)

namespace mirobody { namespace chat {

class Transport {
public:
    virtual ~Transport() = default;

    Transport()                            = default;
    Transport(const Transport&)            = delete;
    Transport& operator=(const Transport&) = delete;

    // A short transport name for logging ("sse").
    virtual const char* name() const = 0;

    // Begin accepting requests: register the route(s) on the Router the
    // transport was constructed with. Called once by ChatService at startup.
    virtual void start() = 0;
};

}}
