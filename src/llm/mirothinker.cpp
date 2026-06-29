#include "llm/mirothinker.hpp"

#include "client/curl_tls.hpp"
#include "llm/sse_parser.hpp"
#include "platform/log.hpp"

#include <curl/curl.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstring>
#include <string>
#include <utility>

namespace mirobody { namespace llm {

namespace {

//------------------------------------------------------------------------------
// Stream state
//------------------------------------------------------------------------------

struct StreamContext {
    SseParser parser;
    std::string text_buffer;         // pending Reply text waiting for a flush
    std::string think_buffer;        // pending Thinking text waiting for a flush
    std::size_t min_chunk_size = 30;

    std::int64_t input_tokens   = 0;
    std::int64_t output_tokens  = 0;
    std::int64_t thought_tokens = 0;
    std::int64_t total_tokens   = 0;

    bool aborted   = false;
    bool done_seen = false;

    const EventHandler* on_event = nullptr;
};

bool dispatch(StreamContext& ctx, const Event& e) {
    if (ctx.aborted) return false;
    if (!(*ctx.on_event)(e)) {
        ctx.aborted = true;
        return false;
    }
    return true;
}

bool flush_text(StreamContext& ctx) {
    if (ctx.text_buffer.empty()) return true;
    Event e;
    e.type    = EventType::Reply;
    e.content = std::move(ctx.text_buffer);
    ctx.text_buffer.clear();
    return dispatch(ctx, e);
}

// Like flush_text, but for buffered reasoning. Flushed before any Reply or
// tool/query event (and at stream end) so the coalesced thinking still arrives
// in order rather than as one tiny event per delta fragment.
bool flush_think(StreamContext& ctx) {
    if (ctx.think_buffer.empty()) return true;
    Event e;
    e.type    = EventType::Thinking;
    e.content = std::move(ctx.think_buffer);
    ctx.think_buffer.clear();
    return dispatch(ctx, e);
}

//------------------------------------------------------------------------------
// JSON helpers
//------------------------------------------------------------------------------

std::string get_string(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return {};
    const auto& m = v[key];
    if (m.IsString()) return std::string{m.GetString(), m.GetStringLength()};
    return {};
}

std::int64_t get_int64(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return 0;
    const auto& m = v[key];
    if (m.IsInt64())  return m.GetInt64();
    if (m.IsInt())    return m.GetInt();
    if (m.IsUint64()) return static_cast<std::int64_t>(m.GetUint64());
    if (m.IsUint())   return static_cast<std::int64_t>(m.GetUint());
    return 0;
}

std::string serialize(const rapidjson::Value& v) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    v.Accept(w);
    return std::string{buf.GetString(), buf.GetSize()};
}

// Strings pass through verbatim; anything else gets JSON-encoded so the
// handler sees a uniform string payload — matching `json.dumps` in Python.
std::string stringify_or_serialize(const rapidjson::Value& v) {
    if (v.IsString()) return std::string{v.GetString(), v.GetStringLength()};
    return serialize(v);
}

//------------------------------------------------------------------------------
// Reasoning-step parsing
//------------------------------------------------------------------------------

void handle_thinking_step(StreamContext& ctx, const rapidjson::Value& step) {
    std::string thought = get_string(step, "thought");
    if (thought.empty()) thought = get_string(step, "text");
    if (thought.empty()) return;
    ctx.think_buffer += thought;
    if (ctx.think_buffer.size() >= ctx.min_chunk_size) {
        flush_think(ctx);
    }
}

void handle_tool_call_step(StreamContext& ctx, const rapidjson::Value& step, std::string tool_id) {
    if (!step.HasMember("tool_call") || !step["tool_call"].IsObject()) return;
    flush_think(ctx);   // reasoning that led here goes out first
    const auto& tc = step["tool_call"];

    if (tool_id.empty()) tool_id = get_string(tc, "id");

    {
        std::string name = get_string(tc, "name");
        if (!name.empty()) {
            Event e;
            e.type    = EventType::QueryTitle;
            e.content = std::move(name);
            e.tool_id = tool_id;
            if (!dispatch(ctx, e)) return;
        }
    }
    if (tc.HasMember("arguments") && !tc["arguments"].IsNull()) {
        Event e;
        e.type    = EventType::QueryArguments;
        e.content = stringify_or_serialize(tc["arguments"]);
        e.tool_id = tool_id;
        if (!dispatch(ctx, e)) return;
    }
    if (tc.HasMember("result") && !tc["result"].IsNull()) {
        Event e;
        e.type    = EventType::QueryDetail;
        e.content = stringify_or_serialize(tc["result"]);
        e.tool_id = tool_id;
        dispatch(ctx, e);
    }
}

void handle_builtin_step(StreamContext& ctx,
                         const rapidjson::Value& step,
                         const std::string& tool_id,
                         const std::string& step_type) {
    flush_think(ctx);   // reasoning that led here goes out first
    Event title;
    title.type    = EventType::QueryTitle;
    title.content = step_type;
    title.tool_id = tool_id;
    if (!dispatch(ctx, title)) return;

    if (step.HasMember(step_type.c_str()) && !step[step_type.c_str()].IsNull()) {
        Event args;
        args.type    = EventType::QueryArguments;
        args.content = stringify_or_serialize(step[step_type.c_str()]);
        args.tool_id = tool_id;
        if (!dispatch(ctx, args)) return;
    }
    if (step.HasMember("result") && !step["result"].IsNull()) {
        Event det;
        det.type    = EventType::QueryDetail;
        det.content = stringify_or_serialize(step["result"]);
        det.tool_id = tool_id;
        dispatch(ctx, det);
    }
}

void handle_delta(StreamContext& ctx, const rapidjson::Value& delta) {
    // Final-answer text content
    if (delta.HasMember("content") && delta["content"].IsString()) {
        std::string txt(delta["content"].GetString(), delta["content"].GetStringLength());
        if (!txt.empty()) {
            flush_think(ctx);   // any pending reasoning precedes the answer text
            ctx.text_buffer.append(txt.data(), txt.size());
            if (ctx.text_buffer.size() >= ctx.min_chunk_size) {
                flush_text(ctx);
            }
        }
    }

    // Reasoning + tool steps
    if (!delta.HasMember("reasoning_steps") || !delta["reasoning_steps"].IsArray()) {
        return;
    }
    for (const auto& step : delta["reasoning_steps"].GetArray()) {
        if (!step.IsObject()) continue;
        std::string step_type = get_string(step, "type");
        std::string tool_id   = get_string(step, "id");
        if (tool_id.empty()) tool_id = get_string(step, "step_id");

        if (step_type == "thinking") {
            handle_thinking_step(ctx, step);
        } else if (step_type == "tool_call") {
            handle_tool_call_step(ctx, step, tool_id);
        } else if (step_type == "web_search" ||
                   step_type == "fetch_url_content" ||
                   step_type == "execute_python" ||
                   step_type == "execute_command") {
            handle_builtin_step(ctx, step, tool_id, step_type);
        }
        if (ctx.aborted) return;
    }
}

//------------------------------------------------------------------------------
// SSE dispatch
//------------------------------------------------------------------------------

void handle_data_event(StreamContext& ctx, const char* payload) {
    if (std::strcmp(payload, "[DONE]") == 0) {
        ctx.done_seen = true;
        return;
    }

    rapidjson::Document doc;
    doc.Parse(payload);
    if (doc.HasParseError() || !doc.IsObject()) {
        mirobody::platform::log_warn("miro-thinker: bad SSE JSON: %.200s", payload);
        return;
    }

    // Usage typically arrives with the last chunk but can ride along with any.
    if (doc.HasMember("usage") && doc["usage"].IsObject()) {
        const auto& u = doc["usage"];
        ctx.input_tokens  += get_int64(u, "prompt_tokens");
        ctx.output_tokens += get_int64(u, "completion_tokens");
        ctx.total_tokens  += get_int64(u, "total_tokens");
        if (u.HasMember("completion_tokens_details") && u["completion_tokens_details"].IsObject()) {
            ctx.thought_tokens += get_int64(u["completion_tokens_details"], "reasoning_tokens");
        }
    }

    if (!doc.HasMember("choices") || !doc["choices"].IsArray()) return;
    const auto& choices = doc["choices"].GetArray();
    if (choices.Empty()) return;
    const auto& choice0 = choices[0];
    if (!choice0.IsObject() || !choice0.HasMember("delta")) return;
    const auto& delta = choice0["delta"];
    if (!delta.IsObject()) return;

    handle_delta(ctx, delta);
}

//------------------------------------------------------------------------------
// Request building
//------------------------------------------------------------------------------

std::string build_request_body(const MiroThinkerOptions& opt,
                               const std::vector<ChatMessage>& messages,
                               const std::string& system_prompt) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();

    w.Key("model");
    w.String(opt.model.data(), static_cast<rapidjson::SizeType>(opt.model.size()));

    w.Key("stream"); w.Bool(true);

    w.Key("messages");
    w.StartArray();
    if (!system_prompt.empty()) {
        w.StartObject();
        w.Key("role");    w.String("system");
        w.Key("content"); w.String(system_prompt.data(),
                                   static_cast<rapidjson::SizeType>(system_prompt.size()));
        w.EndObject();
    }
    for (const auto& m : messages) {
        w.StartObject();
        w.Key("role");    w.String(m.role.data(),    static_cast<rapidjson::SizeType>(m.role.size()));
        w.Key("content"); w.String(m.content.data(), static_cast<rapidjson::SizeType>(m.content.size()));
        w.EndObject();
    }
    w.EndArray();

    w.Key("mcp_servers");
    w.StartArray();
    w.StartObject();
    w.Key("name"); w.String(opt.mcp_server_name.data(),
                            static_cast<rapidjson::SizeType>(opt.mcp_server_name.size()));
    w.Key("url");  w.String(opt.mcp_url.data(),
                            static_cast<rapidjson::SizeType>(opt.mcp_url.size()));
    w.EndObject();
    w.EndArray();

    w.EndObject();
    return std::string{buf.GetString(), buf.GetSize()};
}

//------------------------------------------------------------------------------
// libcurl callbacks
//------------------------------------------------------------------------------

struct CurlContext {
    StreamContext* stream  = nullptr;
    std::string error_buf;          // captured response body when HTTP status is 4xx/5xx
    bool error_mode = false;
};

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* cc = static_cast<CurlContext*>(userdata);
    const size_t n = size * nmemb;

    if (cc->error_mode) {
        cc->error_buf.append(ptr, n);
        return n;
    }
    if (cc->stream->aborted) return 0;

    cc->stream->parser.feed(ptr, n, [&](const void* payload, std::size_t /*length*/) {
        if (cc->stream->aborted) return;
        handle_data_event(*cc->stream, static_cast<const char*>(payload));
    });
    return cc->stream->aborted ? 0 : n;
}

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* cc = static_cast<CurlContext*>(userdata);
    const size_t len = size * nitems;
    // The status line ("HTTP/x.y NNN ...") arrives first; redirects (followed
    // by libcurl) cause additional status lines — the last one wins.
    if (len >= 12 && std::memcmp(buffer, "HTTP/", 5) == 0) {
        const char*  sp  = static_cast<const char*>(std::memchr(buffer, ' ', len));
        const size_t pos = sp ? static_cast<size_t>(sp - buffer) : len;
        if (pos + 4 <= len) {
            int  code = 0;
            bool ok   = true;
            for (size_t i = pos + 1; i < pos + 4; ++i) {
                if (buffer[i] < '0' || buffer[i] > '9') { ok = false; break; }
                code = code * 10 + (buffer[i] - '0');
            }
            if (ok) cc->error_mode = (code >= 400);
        }
    }
    return len;
}

}

//------------------------------------------------------------------------------
// MiroThinkerClient
//------------------------------------------------------------------------------

MiroThinkerClient::MiroThinkerClient(MiroThinkerOptions opt)
    : opt_(std::move(opt)) {}

//------------------------------------------------------------------------------

bool MiroThinkerClient::ainvoke(const std::vector<ChatMessage>& messages,
                                const std::string& system_prompt,
                                const EventHandler& on_event,
                                const UserContext& /*user*/) {
    auto emit_error = [&](std::string msg) {
        Event e; e.type = EventType::Error; e.content = std::move(msg);
        on_event(e);
    };

    if (opt_.api_key.empty()) {
        emit_error("MIROTHINKER_API_KEY is required for MiroThinkerClient functionality.");
        return false;
    }
    if (opt_.mcp_url.empty()) {
        emit_error("MCP_PUBLIC_URL is required: MiroThinker reaches our MCP server over the internet.");
        return false;
    }
    if (messages.empty()) {
        emit_error("Empty message.");
        return false;
    }

    const std::string body = build_request_body(opt_, messages, system_prompt);
    const std::string url  = opt_.base_url + "/chat/completions";

    CURL* curl = curl_easy_init();
    if (!curl) {
        emit_error("curl_easy_init failed");
        return false;
    }

    StreamContext stream;
    stream.min_chunk_size = opt_.min_chunk_size;
    stream.on_event       = &on_event;

    CurlContext cc;
    cc.stream = &stream;

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
    std::string auth = "Authorization: Bearer " + opt_.api_key;
    hdrs = curl_slist_append(hdrs, auth.c_str());

    mirobody::client::configure_tls_trust(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cc);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &cc);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(opt_.connect_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(opt_.request_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (stream.aborted) {
        // Handler said stop; don't surface a fake transport error.
        return false;
    }

    if (rc != CURLE_OK) {
        emit_error(std::string{"transport error: "} + curl_easy_strerror(rc));
        return false;
    }

    if (cc.error_mode) {
        emit_error(cc.error_buf.empty()
            ? std::string{"HTTP "} + std::to_string(http_code)
            : cc.error_buf);
        return false;
    }

    // Drain any trailing event the server didn't terminate with a blank line.
    stream.parser.finish([&](const void* payload, std::size_t /*length*/) {
        if (stream.aborted) return;
        handle_data_event(stream, static_cast<const char*>(payload));
    });
    if (stream.aborted) return false;

    flush_think(stream);
    if (stream.aborted) return false;
    flush_text(stream);
    if (stream.aborted) return false;

    Event cs;
    cs.type                = EventType::CostStatistics;
    cs.cost.model          = opt_.model;
    cs.cost.input_tokens   = stream.input_tokens;
    cs.cost.output_tokens  = stream.output_tokens;
    cs.cost.thought_tokens = stream.thought_tokens;
    cs.cost.total_tokens   = stream.total_tokens;
    cs.cost.total_cost =
        (static_cast<double>(stream.input_tokens) * opt_.input_price +
         static_cast<double>(stream.thought_tokens + stream.output_tokens) * opt_.output_price)
        / 1e6;
    on_event(cs);

    return true;
}

}
}
