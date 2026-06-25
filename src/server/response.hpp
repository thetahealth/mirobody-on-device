#pragma once

// HTTP response-side machinery, split out of server/router.hpp: the SseEvent
// value type and the Response a handler fills in. The async streaming channel
// it hands out lives in server/stream_writer.hpp (included here). router.hpp
// includes this, so #include "server/router.hpp" still pulls in everything;
// include this header directly when you only touch the response side.

#include "compat/cxx11.hpp"
#include "platform/log.hpp"   // log_warn (error())
#include "server/request.hpp" // Request (error()'s log)
#include "server/stream_writer.hpp"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct lws;
struct lws_context;

namespace mirobody { namespace server {

//------------------------------------------------------------------------------
// HTTP response
//------------------------------------------------------------------------------

// One Server-Sent Events message, produced by an SSE feed (see Response::sse).
// All fields are optional: a bare `data` is the common case; `event` names a
// custom event type the client dispatches on; `id` is echoed back as
// Last-Event-ID when the browser reconnects; `retry_ms` (>= 0) overrides the
// client's reconnect delay; `comment` emits a ':'-prefixed line, the usual way
// to send a keep-alive ping that the client ignores. `data` may contain '\n':
// it is split into one "data:" line per source line, which the client rejoins.
struct SseEvent {
    std::string data;
    std::string event;
    std::string id;
    int         retry_ms = -1;
    std::string comment;
};

class Response {
public:
    Response() = default;

    void status(int code) { status_ = code; }
    void content_type(std::string ct) { content_type_ = std::move(ct); }
    void header(std::string name, std::string value) {
        extra_headers_.emplace_back(std::move(name), std::move(value));
    }
    void body(std::string b) { body_ = std::move(b); }
    void json(std::string b) {
        content_type_ = "application/json";
        body_ = std::move(b);
    }
    void text(std::string b) {
        content_type_ = "text/plain; charset=utf-8";
        body_ = std::move(b);
    }

    // Write the project's standard JSON envelope for a FAILURE (success uses
    // ok()):
    //
    //   {"code": <code>, "msg": <msg> [, "data": <data>]}
    //
    // `code` is the non-zero application error code; the route/client and `msg`
    // are logged. `data_json`, when non-empty, is spliced in verbatim as "data"
    // and MUST itself be valid JSON. HTTP status is 200 by default; `status`
    // overrides it for endpoints that must also signal failure at the transport
    // level (e.g. a 401/404 alongside the envelope). Mirrors the Python
    // json_response_with_code helper the frontend expects.
    void error(int code,
               const std::string& msg,
               const std::string& data_json = std::string(),
               int status = 200) {
        this->status(status);
        // Surface the failure in the logs. The router sets request_ before
        // dispatch, so the line names the route and client (uri / ip / ua).
        if (code != 0 || status >= 300) {
            if (request_ != nullptr) {
                platform::log_warn("error %d on %s %s (ip=%s ua=\"%s\"): %s",
                                   code, request_->method.c_str(), request_->path.c_str(),
                                   request_->ip.c_str(), request_->user_agent.c_str(),
                                   msg.c_str());
            } else {
                platform::log_warn("error %d: %s", code, msg.c_str());
            }
        }
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("code");    w.Int(code);
        w.Key("msg");     w.String(msg.data(), static_cast<rapidjson::SizeType>(msg.size()));
        if (!data_json.empty()) {
            // Spliced verbatim; the caller guarantees it is valid JSON.
            w.Key("data");
            w.RawValue(data_json.data(), data_json.size(), rapidjson::kObjectType);
        }
        w.EndObject();
        json(std::string(buf.GetString(), buf.GetSize()));
    }

    // Write the standard success envelope ({"code":0,"msg":"ok"[, "data"]}, HTTP
    // 200). `data_json`, when non-empty, is spliced in verbatim as "data" and
    // MUST itself be valid JSON. Failures use error().
    void ok(const std::string& data_json = std::string()) {
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);

        w.StartObject();
        w.Key("code");    w.Int(0);
        w.Key("msg");     w.String("ok", static_cast<rapidjson::SizeType>(2));
        if (!data_json.empty()) {
            // Spliced verbatim; the caller guarantees it is valid JSON.
            w.Key("data");
            w.RawValue(data_json.data(), data_json.size(), rapidjson::kObjectType);
        }
        w.EndObject();

        json(std::string(buf.GetString(), buf.GetSize()));
    }

    // Streaming response via HTTP chunked transfer-encoding. Instead of a fixed
    // body, register a producer that the router calls repeatedly from its
    // writeable callback: fill `out` with the next chunk of body bytes and
    // return true, or return false to end the response. The content length need
    // not be known up front -- the router emits Transfer-Encoding: chunked. The
    // chunks are written to the socket back to back, with no separators added,
    // so a streamed JSON document must be produced as one continuous byte stream
    // across calls: the producer owns the opening '[', the element separators,
    // and the closing ']'. Return true only with a non-empty `out`; signal
    // end-of-stream by returning false. The producer runs on the service thread,
    // the same context as an ordinary handler.
    using ChunkProducer = std::function<bool(std::string& out)>;

    void stream_json(ChunkProducer producer) {
        content_type_ = "application/json";
        producer_ = std::move(producer);
    }
    void stream(std::string content_type, ChunkProducer producer) {
        content_type_ = std::move(content_type);
        producer_ = std::move(producer);
    }

    // Server-Sent Events: an open-ended text/event-stream feed built on the same
    // chunked transport as stream(). Register a producer the router calls
    // repeatedly from its writeable callback: fill `ev` with the next event and
    // return true, or return false to end the feed. Each event is serialized to
    // the SSE wire format before it goes out, so the producer deals in SseEvent
    // values, not raw "data:" bytes. The Cache-Control and X-Accel-Buffering
    // headers that keep intermediaries from buffering the stream are set for you.
    using SseProducer = std::function<bool(SseEvent& ev)>;

    void sse(SseProducer producer) {
        content_type_ = "text/event-stream";
        header("Cache-Control", "no-cache");
        header("X-Accel-Buffering", "no");
        SseProducer p = std::move(producer);
        producer_ = [p](std::string& out) -> bool {
            SseEvent ev;
            if (!p(ev)) return false;
            out = format_sse(ev);
            return true;
        };
    }

    // Start an asynchronous streaming response. Returns a StreamWriter the
    // handler hands to a worker thread; the worker drives begin()/write()/
    // finish() as data arrives and the router relays it to the client over
    // chunked transfer-encoding without blocking the service thread. Use this to
    // proxy an upstream streaming response (e.g. OpenAI's stream:true SSE) --
    // forward the upstream bytes verbatim. Requires the connection binding the
    // router sets via bind(); not available from a synchronous body.
    std::shared_ptr<StreamWriter> begin_stream() {
        stream_writer_ = std::make_shared<StreamWriter>(ctx_);
        return stream_writer_;
    }

    int status() const { return status_; }
    const std::string& content_type() const { return content_type_; }
    const std::vector<std::pair<std::string, std::string>>& headers() const { return extra_headers_; }
    const std::string& body() const { return body_; }

    bool is_streaming() const { return static_cast<bool>(producer_); }
    const ChunkProducer& producer() const { return producer_; }

    bool is_async_stream() const { return static_cast<bool>(stream_writer_); }
    const std::shared_ptr<StreamWriter>& stream_writer() const { return stream_writer_; }

    // Internal: the router binds the live connection before invoking the handler
    // so begin_stream() can reach the lws context. Not for handler use.
    void bind(lws* wsi, lws_context* ctx) { wsi_ = wsi; ctx_ = ctx; }

    // Internal: the router points this at the in-flight Request before invoking
    // the handler, so error() can name the route/client in its log. Borrowed;
    // valid only for the synchronous duration of the handler call (the only time
    // error() runs). Not for handler use.
    void set_request(const Request* req) { request_ = req; }

private:
    // Serialize one SSE message to its wire form: optional ':' comment, id:,
    // event:, and retry: lines, then one "data:" line per line of `ev.data`,
    // closed by the blank line that terminates the event.
    static std::string format_sse(const SseEvent& ev) {
        std::string f;
        if (!ev.comment.empty()) { f += ": ";      f += ev.comment; f += "\n"; }
        if (!ev.id.empty())      { f += "id: ";     f += ev.id;      f += "\n"; }
        if (!ev.event.empty())   { f += "event: ";  f += ev.event;   f += "\n"; }
        if (ev.retry_ms >= 0)    { f += "retry: ";  f += std::to_string(ev.retry_ms); f += "\n"; }
        for (std::size_t start = 0; !ev.data.empty() && start <= ev.data.size(); ) {
            std::size_t nl = ev.data.find('\n', start);
            std::size_t len = (nl == std::string::npos ? ev.data.size() : nl) - start;
            f += "data: ";
            f += ev.data.substr(start, len);
            f += "\n";
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        f += "\n";
        return f;
    }

    int status_ = 200;
    std::string content_type_ = "application/octet-stream";
    std::vector<std::pair<std::string, std::string>> extra_headers_;
    std::string body_;
    ChunkProducer producer_;

    lws*                          wsi_ = nullptr;
    lws_context*                  ctx_ = nullptr;
    std::shared_ptr<StreamWriter> stream_writer_;
    const Request*                request_ = nullptr;   // borrowed; see set_request
};

}
}
