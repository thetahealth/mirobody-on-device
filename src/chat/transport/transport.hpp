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
//   - WsTransport    GET  /api/chat  -> WebSocket frames      (transport/ws.*)
//   - MqttTransport  broker topic    -> published frames      (transport/mqtt.*)
//
// These protocols share nothing at the connection level (HTTP/WS register routes
// on a server::Router, MQTT connects to a broker), so the interface fixes only
// the lifecycle: construct the transport with what it needs, then start() it.
// ChatService owns one transport per enabled protocol and starts them all.

namespace mirobody { namespace chat {

class Transport {
public:
    virtual ~Transport() = default;

    Transport()                            = default;
    Transport(const Transport&)            = delete;
    Transport& operator=(const Transport&) = delete;

    // A short transport name for logging ("sse", "ws", "mqtt").
    virtual const char* name() const = 0;

    // Begin accepting requests. An HTTP/WS transport registers its route(s) on
    // the Router it was constructed with; an MQTT transport connects to its
    // broker and subscribes. Called once by ChatService at startup.
    virtual void start() = 0;
};

}}
