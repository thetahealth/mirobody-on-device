#include "chat/transport/mqtt.hpp"

#include "chat/dispatcher.hpp"
#include "platform/log.hpp"

namespace mirobody { namespace chat {

MqttTransport::MqttTransport(Dispatcher& dispatcher, const Config& cfg)
    : dispatcher_(dispatcher), cfg_(cfg) {}

void MqttTransport::start() {
    // No MQTT client is linked into the build yet, so there is nothing to start.
    // When one is added, this method would:
    //
    //   1. Read broker settings from cfg_ (URL, request/response topics, creds).
    //      libwebsockets -- already a project dependency -- ships an MQTT client,
    //      so this can run on the same event loop as the HTTP/WS server without a
    //      new third-party dependency.
    //   2. Connect, then subscribe to the request topic.
    //   3. On each message: chat::Packet pkt = chat::Packet::parse(payload); build
    //      an MqttResponder (implements chat::Responder) that publishes each event
    //      to the request's reply topic; stamp session_id/timezone into pkt, then
    //      dispatcher_.dispatch(pkt, uid, responder) -- the same call the SSE/WS
    //      transports make. Correlate
    //      replies via the MQTT5 response-topic / correlation-data properties or a
    //      request id in params.
    //   4. Authenticate at CONNECT (broker credentials / token) or per-message via
    //      a JWT in params -- the bearer-header scheme HTTP/WS use does not apply
    //      to a broker connection.
    //
    // dispatcher_ / cfg_ are held for that future implementation.
    (void)dispatcher_;
    (void)cfg_;
    platform::log_info("chat: mqtt transport not configured (no broker client built); skipping");
}

}}
