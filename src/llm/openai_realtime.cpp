#include "llm/openai_realtime.hpp"

#include "client/websocket_client.hpp"
#include "platform/log.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace llm {

namespace {

//------------------------------------------------------------------------------
// Exchange state
//------------------------------------------------------------------------------

// Shared between the calling (ainvoke) thread and the WebSocket service thread.
// The service thread drives parsing and event dispatch; the calling thread waits
// on `cv` until `done` and then does the final flush + cost on its own. The two
// never touch the buffers concurrently: every dispatch happens on the service
// thread until done is signalled, after which the calling thread joins the
// socket (close()) before flushing, so the join is the happens-before edge.
struct Exchange {
    std::mutex              mu;
    std::condition_variable cv;
    bool done      = false;
    bool errored   = false;
    bool timed_out = false;
    bool aborted   = false;
    bool started   = false;   // turn sent after session.updated (service thread only)
    std::string error_msg;

    // Reply-text coalescing (mirrors GeminiLiveClient): buffer text deltas and
    // emit a single Reply once they reach min_chunk_size.
    std::string text_buffer;
    std::size_t min_chunk_size = 30;

    std::int64_t input_tokens   = 0;
    std::int64_t output_tokens  = 0;
    std::int64_t thought_tokens = 0;
    std::int64_t total_tokens   = 0;

    const EventHandler* on_event = nullptr;

    void signal_done() {
        std::lock_guard<std::mutex> lk(mu);
        done = true;
        cv.notify_all();
    }
    void fail(std::string msg) {
        std::lock_guard<std::mutex> lk(mu);
        if (!done) { errored = true; error_msg = std::move(msg); }
        done = true;
        cv.notify_all();
    }
};

// Returns false once the stream is aborted (handler said stop); the caller
// should then stop producing.
bool dispatch(Exchange& ex, const Event& e) {
    if (ex.aborted) return false;
    if (!(*ex.on_event)(e)) {
        ex.aborted = true;
        ex.signal_done();
        return false;
    }
    return true;
}

bool flush_text(Exchange& ex) {
    if (ex.text_buffer.empty()) return true;
    Event e;
    e.type    = EventType::Reply;
    e.content = std::move(ex.text_buffer);
    ex.text_buffer.clear();
    return dispatch(ex, e);
}

//------------------------------------------------------------------------------
// The queued client turn: the conversation items and the response.create that
// are sent once the server acks our session.update.
//------------------------------------------------------------------------------

struct ClientTurn {
    std::vector<std::string> items;   // conversation.item.create messages
    std::string              response_create;
};

//------------------------------------------------------------------------------
// JSON helpers
//------------------------------------------------------------------------------

std::int64_t get_int64(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return 0;
    const auto& m = v[key];
    if (m.IsInt64())  return m.GetInt64();
    if (m.IsInt())    return m.GetInt();
    if (m.IsUint64()) return static_cast<std::int64_t>(m.GetUint64());
    if (m.IsUint())   return static_cast<std::int64_t>(m.GetUint());
    return 0;
}

//------------------------------------------------------------------------------
// Response parsing
//------------------------------------------------------------------------------

void handle_delta(Exchange& ex, const rapidjson::Value& doc) {
    if (!doc.HasMember("delta") || !doc["delta"].IsString()) return;
    std::string txt(doc["delta"].GetString(), doc["delta"].GetStringLength());
    if (txt.empty()) return;
    ex.text_buffer.append(txt.data(), txt.size());
    if (ex.text_buffer.size() >= ex.min_chunk_size) {
        flush_text(ex);
    }
}

void handle_usage(Exchange& ex, const rapidjson::Value& resp) {
    if (!resp.IsObject() || !resp.HasMember("usage") || !resp["usage"].IsObject()) return;
    const auto& u = resp["usage"];
    ex.input_tokens  = get_int64(u, "input_tokens");
    ex.output_tokens = get_int64(u, "output_tokens");
    ex.total_tokens  = get_int64(u, "total_tokens");
    // Reasoning tokens, when reported, live under output_token_details.
    if (u.HasMember("output_token_details") && u["output_token_details"].IsObject()) {
        ex.thought_tokens = get_int64(u["output_token_details"], "reasoning_tokens");
    }
}

// Parse one server event. The first relevant one is session.created (ignored);
// session.updated acks our setup and triggers the queued client turn. Text
// deltas stream in, response.done carries usage and ends the exchange, and an
// error event fails it.
void handle_server_message(Exchange& ex,
                           client::WebSocketClient& ws,
                           const ClientTurn& turn,
                           const char* payload) {
    if (ex.aborted) return;

    rapidjson::Document doc;
    doc.Parse(payload);
    if (doc.HasParseError() || !doc.IsObject()) {
        mirobody::platform::log_warn("openai-realtime: bad message JSON: %.200s", payload);
        return;
    }

    if (!doc.HasMember("type") || !doc["type"].IsString()) return;
    const char* type = doc["type"].GetString();
    auto is_type = [&](const char* s) { return std::strcmp(type, s) == 0; };

    // Server-side error: surface the message and end the exchange.
    if (is_type("error")) {
        std::string msg = "openai-realtime: server error";
        if (doc.HasMember("error") && doc["error"].IsObject()
                && doc["error"].HasMember("message") && doc["error"]["message"].IsString()) {
            msg = std::string{doc["error"]["message"].GetString(),
                              doc["error"]["message"].GetStringLength()};
        }
        ex.fail(std::move(msg));
        return;
    }

    // Handshake: the server acks session.update, then we send the conversation
    // items and ask for a response. Guarded so it fires exactly once.
    if (is_type("session.updated")) {
        if (!ex.started) {
            ex.started = true;
            for (const auto& item : turn.items) ws.send_text(item.c_str());
            ws.send_text(turn.response_create.c_str());
        }
        return;
    }

    // Text deltas: GA emits response.output_text.delta, the preview surface
    // response.text.delta. Both carry the chunk in `delta`.
    if (is_type("response.output_text.delta") || is_type("response.text.delta")) {
        handle_delta(ex, doc);
        return;
    }

    // response.done ends our one-shot exchange and carries cumulative usage.
    if (is_type("response.done")) {
        if (doc.HasMember("response") && doc["response"].IsObject()) {
            handle_usage(ex, doc["response"]);
        }
        ex.signal_done();
        return;
    }

    // session.created, response.created, response.output_item.*,
    // response.*_text.done, rate_limits.updated, etc. are informational here.
}

//------------------------------------------------------------------------------
// Request building
//------------------------------------------------------------------------------

std::string build_session_update(const OpenAIRealtimeOptions& opt, const std::string& system_prompt) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("type"); w.String("session.update");
    w.Key("session");
    w.StartObject();

    w.Key("modalities");
    w.StartArray();
    w.String("text");
    w.EndArray();

    if (!system_prompt.empty()) {
        w.Key("instructions");
        w.String(system_prompt.data(), static_cast<rapidjson::SizeType>(system_prompt.size()));
    }
    if (opt.temperature > 0.0) {
        w.Key("temperature"); w.Double(opt.temperature);
    }

    w.EndObject();   // session
    w.EndObject();
    return std::string{buf.GetString(), buf.GetSize()};
}

// One conversation.item.create per message. OpenAI-style roles pass through;
// assistant turns use the "text" content part, user/system use "input_text".
std::string build_item(const ChatMessage& m) {
    const char* role;
    const char* content_type;
    if (m.role == "assistant") {
        role = "assistant"; content_type = "text";
    } else if (m.role == "system") {
        role = "system";    content_type = "input_text";
    } else {
        role = "user";      content_type = "input_text";
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("type"); w.String("conversation.item.create");
    w.Key("item");
    w.StartObject();
    w.Key("type"); w.String("message");
    w.Key("role"); w.String(role);
    w.Key("content");
    w.StartArray();
    w.StartObject();
    w.Key("type"); w.String(content_type);
    w.Key("text"); w.String(m.content.data(), static_cast<rapidjson::SizeType>(m.content.size()));
    w.EndObject();
    w.EndArray();
    w.EndObject();   // item
    w.EndObject();
    return std::string{buf.GetString(), buf.GetSize()};
}

std::string build_response_create() {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("type"); w.String("response.create");
    w.Key("response");
    w.StartObject();
    w.Key("modalities");
    w.StartArray();
    w.String("text");
    w.EndArray();
    w.EndObject();   // response
    w.EndObject();
    return std::string{buf.GetString(), buf.GetSize()};
}

ClientTurn build_client_turn(const OpenAIRealtimeOptions& /*opt*/,
                             const std::vector<ChatMessage>& messages) {
    ClientTurn turn;
    turn.items.reserve(messages.size());
    for (const auto& m : messages) turn.items.push_back(build_item(m));
    turn.response_create = build_response_create();
    return turn;
}

//------------------------------------------------------------------------------
// URL building
//------------------------------------------------------------------------------

std::string build_ws_url(const OpenAIRealtimeOptions& opt) {
    if (opt.mode == OpenAIRealtimeMode::OpenAI) {
        std::string host = opt.openai_base_url.empty()
            ? std::string{"wss://api.openai.com/v1/realtime"}
            : opt.openai_base_url;
        return host + "?model=" + opt.model;
    }
    // Azure: {endpoint}/openai/realtime?api-version=...&deployment=...
    return opt.azure_endpoint + "/openai/realtime?api-version=" + opt.api_version
         + "&deployment=" + opt.azure_deployment;
}

}   // namespace

//------------------------------------------------------------------------------
// OpenAIRealtimeClient
//------------------------------------------------------------------------------

OpenAIRealtimeClient::OpenAIRealtimeClient(OpenAIRealtimeOptions opt) : opt_(std::move(opt)) {}

//------------------------------------------------------------------------------

bool OpenAIRealtimeClient::ainvoke(const std::vector<ChatMessage>& messages,
                                   const std::string& system_prompt,
                                   const EventHandler& on_event) {
    auto emit_error = [&](std::string msg) {
        Event e; e.type = EventType::Error; e.content = std::move(msg);
        on_event(e);
    };

    if (opt_.api_key.empty()) {
        emit_error(opt_.mode == OpenAIRealtimeMode::Azure
            ? std::string{"AZURE_OPENAI_API_KEY is required for OpenAI Realtime Azure mode."}
            : std::string{"OPENAI_API_KEY is required for OpenAI Realtime mode."});
        return false;
    }
    if (opt_.mode == OpenAIRealtimeMode::Azure) {
        if (opt_.azure_endpoint.empty() || opt_.azure_deployment.empty()) {
            emit_error("azure_endpoint and azure_deployment are required for OpenAI Realtime Azure mode.");
            return false;
        }
    }
    if (messages.empty()) {
        emit_error("Empty message.");
        return false;
    }

    const std::string url        = build_ws_url(opt_);
    const std::string session_up = build_session_update(opt_, system_prompt);
    const ClientTurn  turn       = build_client_turn(opt_, messages);

    Exchange ex;
    ex.on_event       = &on_event;
    ex.min_chunk_size = opt_.min_chunk_size;

    client::WebSocketClient ws;

    client::WebSocketCallbacks cb;
    cb.on_open = [&]() {
        ws.send_text(session_up.c_str());
    };
    cb.on_message = [&](const void* payload, std::size_t /*length*/, bool /*is_binary*/) {
        handle_server_message(ex, ws, turn, static_cast<const char*>(payload));
    };
    cb.on_error = [&](const char* msg) {
        ex.fail(std::string{"transport error: "} + std::string{msg});
    };
    cb.on_close = [&](int /*code*/, const char* /*reason*/) {
        // A close before response.done leaves `done` set with no error -- the
        // exchange just ends; any buffered text is still flushed below.
        ex.signal_done();
    };

    client::WebSocketOptions wopts;
    wopts.url                  = url;
    wopts.connect_timeout_ms   = opt_.connect_timeout_ms;
    wopts.insecure_skip_verify = opt_.insecure_skip_verify;
    if (opt_.mode == OpenAIRealtimeMode::Azure) {
        wopts.headers.push_back("api-key: " + opt_.api_key);
    } else {
        wopts.headers.push_back("Authorization: Bearer " + opt_.api_key);
    }
    if (opt_.beta_header) {
        wopts.headers.push_back("OpenAI-Beta: realtime=v1");
    }

    if (!ws.connect(std::move(wopts), std::move(cb))) {
        emit_error("openai realtime: failed to start WebSocket connection (bad URL?)");
        return false;
    }

    {
        std::unique_lock<std::mutex> lk(ex.mu);
        const bool finished = ex.cv.wait_for(
            lk, std::chrono::milliseconds(opt_.request_timeout_ms),
            [&] { return ex.done; });
        if (!finished) ex.timed_out = true;
    }

    // Joins the service thread, so no callback runs past this point.
    ws.close();

    if (ex.aborted)   return false;
    if (ex.timed_out) { emit_error("openai realtime: timed out waiting for response"); return false; }
    if (ex.errored)   { emit_error(ex.error_msg.empty() ? std::string{"openai realtime: stream error"}
                                                        : ex.error_msg); return false; }

    flush_text(ex);
    if (ex.aborted) return false;

    Event cs;
    cs.type                = EventType::CostStatistics;
    cs.cost.model          = opt_.model;
    cs.cost.input_tokens   = ex.input_tokens;
    cs.cost.output_tokens  = ex.output_tokens;
    cs.cost.thought_tokens = ex.thought_tokens;
    cs.cost.total_tokens   = ex.total_tokens;
    cs.cost.total_cost =
        (static_cast<double>(ex.input_tokens) * opt_.input_price +
         static_cast<double>(ex.thought_tokens + ex.output_tokens) * opt_.output_price)
        / 1e6;
    on_event(cs);

    return true;
}

}
}
