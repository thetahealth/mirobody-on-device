#include "llm/gemini_live.hpp"

#include "client/websocket_client.hpp"
#include "platform/log.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>

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
    std::string error_msg;

    // Reply-text coalescing (mirrors GeminiClient): buffer text parts and emit a
    // single Reply once they reach min_chunk_size.
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

void handle_part(Exchange& ex, const rapidjson::Value& part) {
    if (!part.IsObject()) return;
    if (!part.HasMember("text") || !part["text"].IsString()) return;

    std::string txt(part["text"].GetString(), part["text"].GetStringLength());
    if (txt.empty()) return;

    // Thought parts route to Thinking events; plain text is the user-facing reply.
    const bool is_thought = part.HasMember("thought") && part["thought"].IsBool()
                            && part["thought"].GetBool();
    if (is_thought) {
        Event e;
        e.type    = EventType::Thinking;
        e.content = txt;
        dispatch(ex, e);
        return;
    }

    ex.text_buffer.append(txt.data(), txt.size());
    if (ex.text_buffer.size() >= ex.min_chunk_size) {
        flush_text(ex);
    }
}

void handle_usage(Exchange& ex, const rapidjson::Value& u) {
    if (!u.IsObject()) return;
    // The Live API reports cumulative counts; overwrite to keep the latest.
    ex.input_tokens   = get_int64(u, "promptTokenCount");
    ex.output_tokens  = get_int64(u, "responseTokenCount");
    if (ex.output_tokens == 0) ex.output_tokens = get_int64(u, "candidatesTokenCount");
    ex.thought_tokens = get_int64(u, "thoughtsTokenCount");
    ex.total_tokens   = get_int64(u, "totalTokenCount");
}

// Parse one server message. On the first message (setupComplete) it sends the
// queued client turn; subsequent messages carry serverContent and usage.
void handle_server_message(Exchange& ex,
                           client::WebSocketClient& ws,
                           const std::string& client_content,
                           const char* payload) {
    if (ex.aborted) return;

    rapidjson::Document doc;
    doc.Parse(payload);
    if (doc.HasParseError() || !doc.IsObject()) {
        mirobody::platform::log_warn("gemini-live: bad message JSON: %.200s", payload);
        return;
    }

    // Handshake: the server acks setup, then we send the conversation turn.
    if (doc.HasMember("setupComplete")) {
        ws.send_text(client_content.c_str());
        return;
    }

    if (doc.HasMember("usageMetadata")) {
        handle_usage(ex, doc["usageMetadata"]);
    }

    if (doc.HasMember("serverContent") && doc["serverContent"].IsObject()) {
        const auto& sc = doc["serverContent"];

        if (sc.HasMember("modelTurn") && sc["modelTurn"].IsObject()) {
            const auto& mt = sc["modelTurn"];
            if (mt.HasMember("parts") && mt["parts"].IsArray()) {
                for (const auto& part : mt["parts"].GetArray()) {
                    handle_part(ex, part);
                    if (ex.aborted) return;
                }
            }
        }

        // turnComplete ends our one-shot exchange; generationComplete /
        // interrupted are informational and ignored.
        if (sc.HasMember("turnComplete") && sc["turnComplete"].IsBool()
                && sc["turnComplete"].GetBool()) {
            ex.signal_done();
        }
    }
}

//------------------------------------------------------------------------------
// Request building
//------------------------------------------------------------------------------

void write_text_part(rapidjson::Writer<rapidjson::StringBuffer>& w, const std::string& text) {
    w.StartObject();
    w.Key("text");
    w.String(text.data(), static_cast<rapidjson::SizeType>(text.size()));
    w.EndObject();
}

// setup.model: AiStudio takes "models/{model}"; Vertex needs the full resource
// path under the project/location.
std::string setup_model(const GeminiLiveOptions& opt) {
    if (opt.mode == GeminiLiveMode::AiStudio) {
        return "models/" + opt.model;
    }
    return "projects/" + opt.gcp_project + "/locations/" + opt.gcp_location
         + "/publishers/google/models/" + opt.model;
}

std::string build_setup_message(const GeminiLiveOptions& opt, const std::string& system_prompt) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("setup");
    w.StartObject();

    const std::string model = setup_model(opt);
    w.Key("model");
    w.String(model.data(), static_cast<rapidjson::SizeType>(model.size()));

    w.Key("generationConfig");
    w.StartObject();
    w.Key("temperature"); w.Double(opt.temperature);
    w.Key("responseModalities");
    w.StartArray();
    w.String("TEXT");
    w.EndArray();
    w.EndObject();

    if (!system_prompt.empty()) {
        w.Key("systemInstruction");
        w.StartObject();
        w.Key("parts");
        w.StartArray();
        write_text_part(w, system_prompt);
        w.EndArray();
        w.EndObject();
    }

    w.EndObject();   // setup
    w.EndObject();
    return std::string{buf.GetString(), buf.GetSize()};
}

std::string build_client_content(const std::vector<ChatMessage>& messages) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("clientContent");
    w.StartObject();
    w.Key("turns");
    w.StartArray();
    for (const auto& m : messages) {
        // OpenAI-style "assistant" maps to Gemini's "model"; everything else is
        // "user" (Gemini doesn't accept "system" inside turns).
        const char* role = (m.role == "assistant" || m.role == "model")
                                ? "model" : "user";
        w.StartObject();
        w.Key("role");
        w.String(role);
        w.Key("parts");
        w.StartArray();
        write_text_part(w, m.content);
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    w.Key("turnComplete"); w.Bool(true);
    w.EndObject();   // clientContent
    w.EndObject();
    return std::string{buf.GetString(), buf.GetSize()};
}

//------------------------------------------------------------------------------
// URL building
//------------------------------------------------------------------------------

std::string build_ws_url(const GeminiLiveOptions& opt) {
    if (opt.mode == GeminiLiveMode::AiStudio) {
        std::string host = opt.ai_studio_base_url.empty()
            ? std::string{"wss://generativelanguage.googleapis.com"}
            : opt.ai_studio_base_url;
        return host + "/ws/google.ai.generativelanguage." + opt.api_version
             + ".GenerativeService.BidiGenerateContent?key=" + opt.api_key;
    }
    std::string host = opt.vertex_base_url.empty()
        ? ("wss://" + opt.gcp_location + "-aiplatform.googleapis.com")
        : opt.vertex_base_url;
    return host + "/ws/google.cloud.aiplatform.v1beta1.LlmBidiService/BidiGenerateContent";
}

}   // namespace

//------------------------------------------------------------------------------
// GeminiLiveClient
//------------------------------------------------------------------------------

GeminiLiveClient::GeminiLiveClient(GeminiLiveOptions opt) : opt_(std::move(opt)) {}

//------------------------------------------------------------------------------

bool GeminiLiveClient::ainvoke(const std::vector<ChatMessage>& messages,
                               const std::string& system_prompt,
                               const EventHandler& on_event) {
    auto emit_error = [&](std::string msg) {
        Event e; e.type = EventType::Error; e.content = std::move(msg);
        on_event(e);
    };

    if (opt_.mode == GeminiLiveMode::AiStudio) {
        if (opt_.api_key.empty()) {
            emit_error("GOOGLE_API_KEY is required for Gemini Live AI Studio mode.");
            return false;
        }
    } else {
        if (opt_.access_token.empty()) {
            emit_error("access_token is required for Gemini Live Vertex mode "
                       "(get one via `gcloud auth print-access-token`).");
            return false;
        }
        if (opt_.gcp_project.empty() || opt_.gcp_location.empty()) {
            emit_error("gcp_project and gcp_location are required for Gemini Live Vertex mode.");
            return false;
        }
    }
    if (messages.empty()) {
        emit_error("Empty message.");
        return false;
    }

    const std::string url            = build_ws_url(opt_);
    const std::string setup_msg      = build_setup_message(opt_, system_prompt);
    const std::string client_content = build_client_content(messages);

    Exchange ex;
    ex.on_event       = &on_event;
    ex.min_chunk_size = opt_.min_chunk_size;

    client::WebSocketClient ws;

    client::WebSocketCallbacks cb;
    cb.on_open = [&]() {
        ws.send_text(setup_msg.c_str());
    };
    cb.on_message = [&](const void* payload, std::size_t length, bool /*is_binary*/) {
        handle_server_message(ex, ws, client_content, static_cast<const char*>(payload));
    };
    cb.on_error = [&](const char* msg) {
        ex.fail(std::string{"transport error: "} + std::string{msg});
    };
    cb.on_close = [&](int /*code*/, const char* /*reason*/) {
        // A close before turnComplete leaves `done` set with no error -- the
        // exchange just ends; any buffered text is still flushed below.
        ex.signal_done();
    };

    client::WebSocketOptions wopts;
    wopts.url                 = url;
    wopts.connect_timeout_ms  = opt_.connect_timeout_ms;
    wopts.insecure_skip_verify = opt_.insecure_skip_verify;
    if (opt_.mode == GeminiLiveMode::Vertex) {
        wopts.headers.push_back("Authorization: Bearer " + opt_.access_token);
    }

    if (!ws.connect(std::move(wopts), std::move(cb))) {
        emit_error("gemini live: failed to start WebSocket connection (bad URL?)");
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
    if (ex.timed_out) { emit_error("gemini live: timed out waiting for response"); return false; }
    if (ex.errored)   { emit_error(ex.error_msg.empty() ? std::string{"gemini live: stream error"}
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
