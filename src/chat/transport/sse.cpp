#include "chat/transport/sse.hpp"

#include "chat/dispatcher.hpp"
#include "chat/event/event.hpp"
#include "chat/packet.hpp"
#include "chat/transport/responder.hpp"
#include "platform/log.hpp"
#include "server/auth.hpp"

#include <rapidjson/document.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mirobody { namespace chat {

namespace {

// Server-Sent Events outbound channel: each event becomes a `data: {json}\n\n`
// chunk on the HTTP StreamWriter; finish() delivers the terminal EndEvent chunk
// and closes the stream atomically (see the chunked-encoding note below).
class SseResponder : public Responder {
public:
    explicit SseResponder(std::shared_ptr<server::StreamWriter> writer)
        : writer_(std::move(writer)) {}

    bool send(const Event& e) override {
        writer_->write(e.to_sse());
        return !writer_->cancelled();
    }

    void finish() override {
        // finish(final_bytes): deliver the terminal "end" chunk and end-of-stream
        // atomically, so the router writes it with LWS_WRITE_HTTP_FINAL and lws
        // emits the chunked terminator. write()+finish() would race: the router
        // can drain the "end" chunk before finish() lands, then has no data left
        // to carry FINAL and the body is truncated (ERR_INCOMPLETE_CHUNKED_ENCODING).
        writer_->finish(EndEvent().to_sse());
    }

private:
    std::shared_ptr<server::StreamWriter> writer_;
};

// Add a string member to a JSON object iff the value is non-empty.
void add_str(rapidjson::Value& obj, const char* key, const std::string& val,
             rapidjson::Document::AllocatorType& a) {
    if (val.empty()) return;
    obj.AddMember(rapidjson::StringRef(key),
                  rapidjson::Value(val.c_str(), static_cast<rapidjson::SizeType>(val.size()), a), a);
}

// Set obj[key] = val, overwriting any existing member (unlike add_str, which
// would append a duplicate). Empty val is left untouched, so a header value
// overrides a body field only when actually present.
void set_str(rapidjson::Value& obj, const char* key, const std::string& val,
             rapidjson::Document::AllocatorType& a) {
    if (val.empty()) return;
    rapidjson::Value::MemberIterator it = obj.FindMember(key);
    if (it != obj.MemberEnd()) {
        it->value.SetString(val.c_str(), static_cast<rapidjson::SizeType>(val.size()), a);
    } else {
        obj.AddMember(rapidjson::StringRef(key),
                      rapidjson::Value(val.c_str(), static_cast<rapidjson::SizeType>(val.size()), a), a);
    }
}

}   // namespace

SseTransport::SseTransport(server::Router& router, Dispatcher& dispatcher, const jwt::Jwt& jwt)
    : router_(router), dispatcher_(dispatcher), jwt_(jwt) {}

void SseTransport::start() {
    // POST /api/chat — stream a chat response as Server-Sent Events over a single
    // request. Authenticated by bearer JWT (header or ?token= query, per
    // require_auth). The body names an "agent" to run (chat::agent_registry).
    router_.post("/api/chat", server::require_auth(jwt_,
        [this](const server::Request& req, server::Response& res) {
            handle(req, res);
        }));
}

//------------------------------------------------------------------------------

bool SseTransport::parse_body(const server::Request& req, Packet& out, server::Response& res) {
    // The body is either JSON (the default) or an HTML form: a browser posts
    // application/x-www-form-urlencoded, and a file upload posts
    // multipart/form-data. Both encodings normalize to the same params object;
    // uploaded file bytes ride along as Packet attachments (stored later by the
    // dispatcher), never inside the JSON params. Request parses either form
    // shape (see server/request.hpp); we just read the fields and file parts.
    if (req.is_form_body()) {
        Packet pkt(kOpChat);
        rapidjson::Document::AllocatorType& a = pkt.allocator();
        rapidjson::Value& p = pkt.params_mut();

        add_str(p, "agent",      req.form_str("agent"),      a);
        add_str(p, "provider",   req.form_str("provider"),   a);
        add_str(p, "language",   req.form_str("language"),   a);
        add_str(p, "timezone",   req.form_str("timezone"),   a);
        add_str(p, "session_id", req.form_str("session_id"), a);
        add_str(p, "question",   req.form_str("question"),   a);

        // "messages" rides as a JSON-encoded array string in a form field.
        const std::string messages_json = req.form_str("messages");
        if (!messages_json.empty()) {
            rapidjson::Document md;
            if (!md.Parse(messages_json.c_str()).HasParseError() && md.IsArray()) {
                rapidjson::Value msgs;
                msgs.CopyFrom(md, a);
                p.AddMember(rapidjson::StringRef("messages"), msgs, a);
            }
        }

        // Uploaded files: attach the raw bytes; the dispatcher's preprocess
        // stores them and rewrites params.files. No storage logic here.
        std::vector<server::Request::FilePart>& files = req.form_files();
        platform::log_debug("chat[1/sse]: form parsed %lu file part(s)",
                            (unsigned long)files.size());
        for (std::size_t i = 0; i < files.size(); ++i) {
            server::Request::FilePart& fp = files[i];
            platform::log_debug("chat[1/sse]:   part[%lu] '%s' (%s, %lu bytes)",
                                (unsigned long)i, fp.filename.c_str(),
                                fp.content_type.c_str(), (unsigned long)fp.data.size());
            Attachment att;
            att.filename  = fp.filename;
            att.mime_type = fp.content_type;
            att.data      = std::move(fp.data);
            pkt.add_attachment(std::move(att));
        }

        out = std::move(pkt);
        return true;
    }

    // JSON body (application/json, or anything not recognized as a form): adopt
    // the parsed object as the packet's params.
    rapidjson::Document d;
    if (d.Parse(req.body.c_str()).HasParseError() || !d.IsObject()) {
        res.error(-1, "invalid request body", {}, 400);
        return false;
    }
    out = Packet(kOpChat, std::move(d));
    return true;
}

//------------------------------------------------------------------------------

void SseTransport::handle(const server::Request& req, server::Response& res) {
    Packet pkt;
    if (!parse_body(req, pkt, res)) return;

    // Stamp the transport-level turn context into the packet so the dispatcher
    // reads it from one place: the X-Session-Id / X-Timezone headers (or their
    // query/body fallbacks) override any matching body field. user_id is the
    // exception -- it is the JWT-verified identity, passed to dispatch separately
    // so a request body can never forge it.
    {
        rapidjson::Document::AllocatorType& a = pkt.allocator();
        rapidjson::Value& p = pkt.params_mut();
        set_str(p, "session_id", req.session_id, a);
        set_str(p, "timezone",   req.timezone,   a);
        set_str(p, "accept_language", req.accept_language, a);
    }
    const std::int64_t uid = req.user_id;

    res.header("Cache-Control", "no-cache");
    res.header("X-Accel-Buffering", "no");
    std::shared_ptr<server::StreamWriter> writer = res.begin_stream();

    // Run the turn on a worker thread: the dispatcher fires the responder on the
    // calling thread as upstream bytes arrive, so it must not block the lws
    // service loop. The Packet is move-only, so hand it to the thread via a
    // shared_ptr. `dispatcher` is stable (owned by the service, which outlives
    // the server).
    Dispatcher* dispatcher = &dispatcher_;
    std::shared_ptr<Packet> pktp = std::make_shared<Packet>(std::move(pkt));
    std::thread([writer, pktp, uid, dispatcher]() {
        writer->begin(200, "text/event-stream");
        SseResponder responder(writer);
        dispatcher->dispatch(*pktp, uid, responder);
    }).detach();
}

}}
