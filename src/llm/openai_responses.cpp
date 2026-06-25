#include "llm/openai_responses.hpp"

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
    std::string text_buffer;
    std::size_t min_chunk_size = 30;

    std::int64_t input_tokens   = 0;
    std::int64_t output_tokens  = 0;
    std::int64_t thought_tokens = 0;
    std::int64_t total_tokens   = 0;

    std::string response_id;
    bool aborted = false;

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

std::string stringify_or_serialize(const rapidjson::Value& v) {
    if (v.IsString()) return std::string{v.GetString(), v.GetStringLength()};
    return serialize(v);
}

//------------------------------------------------------------------------------
// Event handling
//------------------------------------------------------------------------------

void handle_output_item(StreamContext& ctx, const rapidjson::Value& item, bool done) {
    if (!item.IsObject()) return;
    std::string item_type = get_string(item, "type");
    if (item_type != "function_call" && item_type != "mcp_call" && item_type != "tool_call") {
        return;  // ignore reasoning/message item shells — text streams via the delta events
    }
    std::string id   = get_string(item, "id");
    std::string name = get_string(item, "name");

    if (!name.empty()) {
        Event title;
        title.type    = EventType::QueryTitle;
        title.content = name;
        title.tool_id = id;
        if (!dispatch(ctx, title)) return;
    }
    if (done) {
        if (item.HasMember("arguments") && !item["arguments"].IsNull()) {
            Event args;
            args.type    = EventType::QueryArguments;
            args.content = stringify_or_serialize(item["arguments"]);
            args.tool_id = id;
            if (!dispatch(ctx, args)) return;
        }
        // MCP completions sometimes inline the result on the item.
        if (item.HasMember("result") && !item["result"].IsNull()) {
            Event det;
            det.type    = EventType::QueryDetail;
            det.content = stringify_or_serialize(item["result"]);
            det.tool_id = id;
            dispatch(ctx, det);
        }
    }
}

void handle_event(StreamContext& ctx, const rapidjson::Document& doc) {
    std::string ev = get_string(doc, "type");

    // Text deltas — buffered + coalesced.
    if (ev == "response.output_text.delta") {
        std::string delta = get_string(doc, "delta");
        if (!delta.empty()) {
            ctx.text_buffer.append(delta);
            if (ctx.text_buffer.size() >= ctx.min_chunk_size) flush_text(ctx);
        }
        return;
    }
    if (ev == "response.output_text.done") {
        flush_text(ctx);
        return;
    }

    // Reasoning narrative (o1/o3-style).
    if (ev == "response.reasoning_text.delta") {
        std::string delta = get_string(doc, "delta");
        if (!delta.empty()) {
            Event e; e.type = EventType::Thinking; e.content = std::move(delta);
            dispatch(ctx, e);
        }
        return;
    }

    // Tool-call lifecycle.
    if (ev == "response.output_item.added" || ev == "response.output_item.done") {
        if (doc.HasMember("item") && doc["item"].IsObject()) {
            handle_output_item(ctx, doc["item"], /*done=*/ev == "response.output_item.done");
        }
        return;
    }

    // Function arguments and MCP arguments arrive as a separate finalization
    // event with the full string. Use it when the item-done event didn't
    // carry args (the SDK route prefers these specific events).
    if (ev == "response.function_call_arguments.done" || ev == "response.mcp_call_arguments.done") {
        std::string item_id = get_string(doc, "item_id");
        std::string arguments;
        if (doc.HasMember("arguments")) {
            arguments = stringify_or_serialize(doc["arguments"]);
        }
        if (!arguments.empty()) {
            Event args;
            args.type    = EventType::QueryArguments;
            args.content = std::move(arguments);
            args.tool_id = std::move(item_id);
            dispatch(ctx, args);
        }
        return;
    }
    if (ev == "response.mcp_call.completed") {
        std::string item_id = get_string(doc, "item_id");
        if (doc.HasMember("result") && !doc["result"].IsNull()) {
            Event det;
            det.type    = EventType::QueryDetail;
            det.content = stringify_or_serialize(doc["result"]);
            det.tool_id = std::move(item_id);
            dispatch(ctx, det);
        }
        return;
    }

    if (ev == "response.completed") {
        if (doc.HasMember("response") && doc["response"].IsObject()) {
            const auto& resp = doc["response"];
            ctx.response_id = get_string(resp, "id");
            if (resp.HasMember("usage") && resp["usage"].IsObject()) {
                const auto& u = resp["usage"];
                ctx.input_tokens  += get_int64(u, "input_tokens");
                ctx.output_tokens += get_int64(u, "output_tokens");
                ctx.total_tokens  += get_int64(u, "total_tokens");
                if (u.HasMember("output_tokens_details") && u["output_tokens_details"].IsObject()) {
                    ctx.thought_tokens += get_int64(u["output_tokens_details"], "reasoning_tokens");
                }
            }
        }
        return;
    }

    if (ev == "response.failed") {
        std::string msg;
        if (doc.HasMember("response") && doc["response"].IsObject()) {
            const auto& resp = doc["response"];
            if (resp.HasMember("error") && !resp["error"].IsNull()) {
                msg = stringify_or_serialize(resp["error"]);
            }
        }
        if (msg.empty()) msg = "response failed";
        Event e; e.type = EventType::Error; e.content = std::move(msg);
        dispatch(ctx, e);
        return;
    }

    // Other events (`response.created`, `response.in_progress`, individual
    // `response.output_item.delta` shells, etc.) are not relevant to a
    // text-streaming debug client; drop them silently.
}

void handle_data_event(StreamContext& ctx, const char* payload) {
    if (std::strcmp(payload, "[DONE]") == 0) return;

    rapidjson::Document doc;
    doc.Parse(payload);
    if (doc.HasParseError() || !doc.IsObject()) {
        mirobody::platform::log_warn("openai-responses: bad SSE JSON: %.200s", payload);
        return;
    }
    handle_event(ctx, doc);
}

//------------------------------------------------------------------------------
// Request building
//------------------------------------------------------------------------------

std::string build_request_body(const OpenAIResponsesOptions& opt,
                               const std::vector<ChatMessage>& messages,
                               const std::string& system_prompt) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();

    w.Key("model");
    w.String(opt.model.data(), static_cast<rapidjson::SizeType>(opt.model.size()));

    w.Key("stream"); w.Bool(true);
    w.Key("store");  w.Bool(opt.store);

    if (!system_prompt.empty()) {
        w.Key("instructions");
        w.String(system_prompt.data(),
                 static_cast<rapidjson::SizeType>(system_prompt.size()));
    }

    if (!opt.previous_response_id.empty()) {
        w.Key("previous_response_id");
        w.String(opt.previous_response_id.data(),
                 static_cast<rapidjson::SizeType>(opt.previous_response_id.size()));
    }

    // Responses API takes `input` as either a string or an array of items.
    // We always use the array form so multi-turn / multi-message inputs work.
    w.Key("input");
    w.StartArray();
    for (const auto& m : messages) {
        w.StartObject();
        w.Key("role");    w.String(m.role.data(),    static_cast<rapidjson::SizeType>(m.role.size()));
        w.Key("content"); w.String(m.content.data(), static_cast<rapidjson::SizeType>(m.content.size()));
        w.EndObject();
    }
    w.EndArray();

    w.EndObject();
    return std::string{buf.GetString(), buf.GetSize()};
}

//------------------------------------------------------------------------------
// libcurl callbacks
//------------------------------------------------------------------------------

struct CurlContext {
    StreamContext* stream = nullptr;
    std::string error_buf;
    bool error_mode = false;
};

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* cc = static_cast<CurlContext*>(userdata);
    const size_t n = size * nmemb;
    if (cc->error_mode) { cc->error_buf.append(ptr, n); return n; }
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
// OpenAIResponsesClient
//------------------------------------------------------------------------------

OpenAIResponsesClient::OpenAIResponsesClient(OpenAIResponsesOptions opt)
    : opt_(std::move(opt)) {}

//------------------------------------------------------------------------------

bool OpenAIResponsesClient::ainvoke(const std::vector<ChatMessage>& messages,
                                    const std::string& system_prompt,
                                    const EventHandler& on_event,
                                    const UserContext& /*user*/) {
    auto emit_error = [&](std::string msg) {
        Event e; e.type = EventType::Error; e.content = std::move(msg);
        on_event(e);
    };

    if (opt_.api_key.empty()) {
        emit_error("API key is required for OpenAIResponsesClient functionality.");
        return false;
    }
    if (messages.empty()) {
        emit_error("Empty message.");
        return false;
    }

    const std::string body = build_request_body(opt_, messages, system_prompt);
    const std::string url  = opt_.base_url + "/responses";

    CURL* curl = curl_easy_init();
    if (!curl) { emit_error("curl_easy_init failed"); return false; }

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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,        static_cast<long>(opt_.request_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (stream.aborted) return false;

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

    stream.parser.finish([&](const void* payload, std::size_t /*length*/) {
        if (stream.aborted) return;
        handle_data_event(stream, static_cast<const char*>(payload));
    });
    if (stream.aborted) return false;

    flush_text(stream);
    if (stream.aborted) return false;

    last_response_id_ = stream.response_id;

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
