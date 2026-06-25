#pragma once

// MqttTransport -- the MQTT transport for the chat service.
//
// PLACEHOLDER. The build links no MQTT client yet, so start() is inert: it logs
// that the transport is disabled and returns. The class exists so the transport
// has a home and so the wiring (ChatService owning and starting it) is identical
// to the HTTP/WS transports once a broker client is added -- implementing MQTT is
// then purely additive, with no change to ChatService or the Dispatcher.
//
// Integration sketch (see mqtt.cpp for the detailed notes): connect to the
// broker, subscribe to a request topic, parse each message with
// chat::Packet::parse ({code, params}), and for each request build an
// MqttResponder bound to the reply topic and call dispatcher_.dispatch(pkt, uid,
// sid, responder) -- exactly what the SSE/WS transports do, minus the HTTP/WS
// connection plumbing.

#include "chat/transport/transport.hpp"
#include "config/config.hpp"

namespace mirobody { namespace chat {

class Dispatcher;

class MqttTransport : public Transport {
public:
    // `dispatcher`/`cfg` are borrowed and must outlive the transport. `cfg` will
    // carry the broker settings (URL, topics, credentials) once those are added.
    MqttTransport(Dispatcher& dispatcher, const Config& cfg);

    const char* name() const override { return "mqtt"; }
    void        start() override;   // currently inert (no broker client built)

private:
    Dispatcher&   dispatcher_;
    const Config& cfg_;
};

}}
