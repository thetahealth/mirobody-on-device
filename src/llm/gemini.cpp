#include "llm/gemini.hpp"

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
#include <vector>

namespace mirobody { namespace llm {

namespace {

//------------------------------------------------------------------------------
// Stream state
//------------------------------------------------------------------------------

// One functionCall the model emitted this turn. Gemini sends each call as a
// whole part (not fragmented), so a part maps to a complete call.
struct FunctionCall {
    std::string id;        // synthetic when the model omits one (for event tool_id)
    std::string name;
    std::string args_json; // the call's "args" object, serialized ("{}" if absent)
};

struct StreamContext {
    SseParser parser;
    std::string text_buffer;
    std::string full_text;          // the turn's complete assistant text (for replay)
    std::size_t min_chunk_size = 30;

    std::int64_t input_tokens   = 0;
    std::int64_t output_tokens  = 0;
    std::int64_t thought_tokens = 0;
    std::int64_t total_tokens   = 0;

    // Synthetic id counter for tool_call events when the model omits ids.
    // Seeded by the caller from the previous round's value: a fresh
    // StreamContext is built per tool round, and a reset counter would hand
    // round 2's call the same "fc_1" as round 1's -- consumers keying on
    // tool_id (the web UI's tool cards) would merge the two calls.
    int function_call_seq = 0;
    std::vector<FunctionCall> calls;

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

//------------------------------------------------------------------------------
// Response parsing
//------------------------------------------------------------------------------

void handle_part(StreamContext& ctx, const rapidjson::Value& part) {
    if (!part.IsObject()) return;

    // Thought parts: keep for context fidelity but route to Thinking events.
    const bool is_thought = part.HasMember("thought") && part["thought"].IsBool() && part["thought"].GetBool();

    if (part.HasMember("text") && part["text"].IsString()) {
        std::string txt(part["text"].GetString(), part["text"].GetStringLength());
        if (txt.empty()) return;

        if (is_thought) {
            Event e;
            e.type    = EventType::Thinking;
            e.content = txt;
            dispatch(ctx, e);
            return;
        }

        ctx.text_buffer.append(txt.data(), txt.size());
        ctx.full_text.append(txt.data(), txt.size());
        if (ctx.text_buffer.size() >= ctx.min_chunk_size) {
            flush_text(ctx);
        }
        return;
    }

    if (part.HasMember("functionCall") && part["functionCall"].IsObject()) {
        // Flush any buffered text first so events stay in order.
        flush_text(ctx);

        const auto& fc = part["functionCall"];
        std::string name = get_string(fc, "name");
        std::string id   = get_string(fc, "id");
        if (id.empty()) {
            id = "fc_" + std::to_string(++ctx.function_call_seq);
        }

        FunctionCall call;
        call.id   = id;
        call.name = name;
        call.args_json = (fc.HasMember("args") && !fc["args"].IsNull())
                       ? serialize(fc["args"]) : std::string{"{}"};

        Event title;
        title.type    = EventType::QueryTitle;
        title.content = name;
        title.tool_id = id;
        if (!dispatch(ctx, title)) return;

        Event args;
        args.type    = EventType::QueryArguments;
        args.content = call.args_json;
        args.tool_id = id;
        if (!dispatch(ctx, args)) return;

        ctx.calls.push_back(std::move(call));
    }
}

void handle_chunk(StreamContext& ctx, const rapidjson::Value& doc) {
    // Usage metadata may ride along with any chunk; the final one usually
    // carries the complete count. Gemini reports each chunk's snapshot so we
    // overwrite rather than accumulate to match the Python implementation.
    if (doc.HasMember("usageMetadata") && doc["usageMetadata"].IsObject()) {
        const auto& u = doc["usageMetadata"];
        ctx.input_tokens   = get_int64(u, "promptTokenCount");
        ctx.output_tokens  = get_int64(u, "candidatesTokenCount");
        ctx.thought_tokens = get_int64(u, "thoughtsTokenCount");
        ctx.total_tokens   = get_int64(u, "totalTokenCount");
    }

    if (!doc.HasMember("candidates") || !doc["candidates"].IsArray()) return;
    for (const auto& cand : doc["candidates"].GetArray()) {
        if (!cand.IsObject() || !cand.HasMember("content")) continue;
        const auto& content = cand["content"];
        if (!content.IsObject() || !content.HasMember("parts") || !content["parts"].IsArray()) continue;
        for (const auto& part : content["parts"].GetArray()) {
            handle_part(ctx, part);
            if (ctx.aborted) return;
        }
    }
}

void handle_data_event(StreamContext& ctx, const char* payload) {
    // Gemini's streamGenerateContent with alt=sse doesn't send [DONE].
    if (std::strcmp(payload, "[DONE]") == 0) return;

    rapidjson::Document doc;
    doc.Parse(payload);
    if (doc.HasParseError() || !doc.IsObject()) {
        mirobody::platform::log_warn("gemini: bad SSE JSON: %.200s", payload);
        return;
    }
    handle_chunk(ctx, doc);
}

//------------------------------------------------------------------------------
// Request building
//------------------------------------------------------------------------------

typedef rapidjson::Writer<rapidjson::StringBuffer> JsonWriter;

std::string finish_buf(rapidjson::StringBuffer& buf) {
    return std::string{buf.GetString(), buf.GetSize()};
}

void write_part_text(JsonWriter& w, const std::string& text) {
    w.StartObject();
    w.Key("text");
    w.String(text.data(), static_cast<rapidjson::SizeType>(text.size()));
    w.EndObject();
}

// {"role": <user|model>, "parts": [{"text": <content>}]}
std::string make_content_text(const std::string& role, const std::string& content) {
    rapidjson::StringBuffer buf;
    JsonWriter w(buf);
    w.StartObject();
    w.Key("role"); w.String(role.data(), static_cast<rapidjson::SizeType>(role.size()));
    w.Key("parts");
    w.StartArray();
    write_part_text(w, content);
    w.EndArray();
    w.EndObject();
    return finish_buf(buf);
}

// Standard base64 (RFC 4648) of raw bytes -- Gemini's inlineData.data wants the
// payload base64-encoded. Kept local so the llm layer doesn't depend on storage.
std::string base64_encode_bytes(const std::string& in) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const unsigned n = (static_cast<unsigned char>(in[i])     << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8)  |
                            static_cast<unsigned char>(in[i + 2]);
        out.push_back(tbl[(n >> 18) & 63]); out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);  out.push_back(tbl[n & 63]);
    }
    if (i + 1 == in.size()) {
        const unsigned n = static_cast<unsigned char>(in[i]) << 16;
        out.push_back(tbl[(n >> 18) & 63]); out.push_back(tbl[(n >> 12) & 63]);
        out.push_back('='); out.push_back('=');
    } else if (i + 2 == in.size()) {
        const unsigned n = (static_cast<unsigned char>(in[i])     << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8);
        out.push_back(tbl[(n >> 18) & 63]); out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);  out.push_back('=');
    }
    return out;
}

// Like make_content_text, but follows the text part with one inlineData part per
// attached file (base64 bytes + mimeType) so the model reads the file directly.
std::string make_content_with_files(const std::string& role, const std::string& content,
                                    const std::vector<FilePart>& files) {
    rapidjson::StringBuffer buf;
    JsonWriter w(buf);
    w.StartObject();
    w.Key("role"); w.String(role.data(), static_cast<rapidjson::SizeType>(role.size()));
    w.Key("parts");
    w.StartArray();
    if (!content.empty()) write_part_text(w, content);
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (files[i].data.empty()) continue;
        const std::string b64  = base64_encode_bytes(files[i].data);
        const std::string mime = files[i].mime_type.empty()
            ? std::string("application/octet-stream") : files[i].mime_type;
        w.StartObject();
        w.Key("inlineData");
        w.StartObject();
        w.Key("mimeType"); w.String(mime.data(), static_cast<rapidjson::SizeType>(mime.size()));
        w.Key("data");     w.String(b64.data(),  static_cast<rapidjson::SizeType>(b64.size()));
        w.EndObject();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return finish_buf(buf);
}

// The model turn that issued the calls, replayed so the follow-up request keeps
// the functionCall/functionResponse pairing Gemini expects.
std::string make_model_function_call_turn(const std::string& text,
                                          const std::vector<FunctionCall>& calls) {
    rapidjson::StringBuffer buf;
    JsonWriter w(buf);
    w.StartObject();
    w.Key("role"); w.String("model");
    w.Key("parts");
    w.StartArray();
    if (!text.empty()) write_part_text(w, text);
    for (std::size_t i = 0; i < calls.size(); ++i) {
        const FunctionCall& c = calls[i];
        const std::string args = c.args_json.empty() ? std::string{"{}"} : c.args_json;
        w.StartObject();
        w.Key("functionCall");
        w.StartObject();
        w.Key("name"); w.String(c.name.data(), static_cast<rapidjson::SizeType>(c.name.size()));
        w.Key("args"); w.RawValue(args.data(), args.size(), rapidjson::kObjectType);
        w.EndObject();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return finish_buf(buf);
}

// {"role":"user","parts":[{"functionResponse":{"name":..,"response":{...}}}, ...]}
std::string make_function_response_turn(const std::vector<FunctionCall>& calls,
                                        const std::vector<std::string>& results) {
    rapidjson::StringBuffer buf;
    JsonWriter w(buf);
    w.StartObject();
    w.Key("role"); w.String("user");
    w.Key("parts");
    w.StartArray();
    for (std::size_t i = 0; i < calls.size(); ++i) {
        const FunctionCall& c = calls[i];
        const std::string& result = results[i];

        w.StartObject();
        w.Key("functionResponse");
        w.StartObject();
        w.Key("name"); w.String(c.name.data(), static_cast<rapidjson::SizeType>(c.name.size()));
        w.Key("response");
        // Gemini requires `response` to be an object. Use the tool's result when
        // it is one; otherwise wrap the raw string under "result".
        rapidjson::Document parsed;
        if (!result.empty() && !parsed.Parse(result.c_str()).HasParseError() && parsed.IsObject()) {
            w.RawValue(result.data(), result.size(), rapidjson::kObjectType);
        } else {
            w.StartObject();
            w.Key("result");
            w.String(result.data(), static_cast<rapidjson::SizeType>(result.size()));
            w.EndObject();
        }
        w.EndObject();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return finish_buf(buf);
}

std::string build_request_body(const GeminiOptions& opt,
                               const std::vector<std::string>& content_objs,
                               const std::string& system_prompt,
                               bool tools_enabled) {
    rapidjson::StringBuffer buf;
    JsonWriter w(buf);
    w.StartObject();

    // systemInstruction (optional). Gemini takes it as a top-level field
    // with a `parts` array, NOT as a role=system message in `contents`.
    if (!system_prompt.empty()) {
        w.Key("systemInstruction");
        w.StartObject();
        w.Key("parts");
        w.StartArray();
        write_part_text(w, system_prompt);
        w.EndArray();
        w.EndObject();
    }

    w.Key("contents");
    w.StartArray();
    for (const auto& c : content_objs) {
        w.RawValue(c.data(), c.size(), rapidjson::kObjectType);
    }
    w.EndArray();

    if (tools_enabled) {
        // Wrap the bare functionDeclarations array in the tools envelope.
        w.Key("tools");
        w.StartArray();
        w.StartObject();
        w.Key("functionDeclarations");
        w.RawValue(opt.tools_json.data(), opt.tools_json.size(), rapidjson::kArrayType);
        w.EndObject();
        w.EndArray();
    }

    w.Key("generationConfig");
    w.StartObject();
    w.Key("temperature"); w.Double(opt.temperature);
    w.EndObject();

    w.EndObject();
    return finish_buf(buf);
}

//------------------------------------------------------------------------------
// URL building
//------------------------------------------------------------------------------

std::string build_url(const GeminiOptions& opt) {
    if (opt.mode == GeminiMode::AiStudio) {
        std::string host = opt.ai_studio_base_url.empty()
            ? std::string{"https://generativelanguage.googleapis.com"}
            : opt.ai_studio_base_url;
        // .../{api_version}/models/{model}:streamGenerateContent?alt=sse&key={api_key}
        std::string url = host + "/" + opt.api_version + "/models/" + opt.model
                        + ":streamGenerateContent?alt=sse&key=" + opt.api_key;
        return url;
    }
    // Vertex: the version is "v1" in practice; AiStudio's "v1beta" default
    // would 404 on Vertex. We pick v1 unconditionally for Vertex.
    std::string host = opt.vertex_base_url.empty()
        ? ("https://" + opt.gcp_location + "-aiplatform.googleapis.com")
        : opt.vertex_base_url;
    std::string url = host
        + "/v1/projects/" + opt.gcp_project
        + "/locations/"   + opt.gcp_location
        + "/publishers/google/models/" + opt.model
        + ":streamGenerateContent?alt=sse";
    return url;
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
    // Status line: "HTTP/x.y CODE ...". Parse the 3-digit code after the first
    // space, directly off the curl buffer (no allocation).
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

//------------------------------------------------------------------------------
// One streaming request
//------------------------------------------------------------------------------

struct RequestOutcome {
    bool        init_failed = false;
    CURLcode    rc          = CURLE_OK;
    long        http_code   = 0;
    bool        http_error  = false;
    std::string http_error_body;
};

RequestOutcome run_request(const GeminiOptions& opt,
                           const std::string& url,
                           const std::string& body,
                           StreamContext& stream) {
    RequestOutcome out;

    CURL* curl = curl_easy_init();
    if (!curl) { out.init_failed = true; return out; }

    CurlContext cc;
    cc.stream = &stream;

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
    std::string auth;
    if (opt.mode == GeminiMode::Vertex) {
        auth = "Authorization: Bearer " + opt.access_token;
        hdrs = curl_slist_append(hdrs, auth.c_str());
    }

    mirobody::client::configure_tls_trust(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cc);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &cc);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(opt.connect_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,        static_cast<long>(opt.request_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    out.rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    out.http_error      = cc.error_mode;
    out.http_error_body = std::move(cc.error_buf);

    if (!stream.aborted && !out.http_error && out.rc == CURLE_OK) {
        stream.parser.finish([&](const void* payload, std::size_t /*length*/) {
            if (stream.aborted) return;
            handle_data_event(stream, static_cast<const char*>(payload));
        });
    }
    return out;
}

}

//------------------------------------------------------------------------------
// GeminiClient
//------------------------------------------------------------------------------

GeminiClient::GeminiClient(GeminiOptions opt) : opt_(std::move(opt)) {}

//------------------------------------------------------------------------------

bool GeminiClient::ainvoke(const std::vector<ChatMessage>& messages,
                           const std::string& system_prompt,
                           const EventHandler& on_event,
                           const UserContext& user) {
    std::size_t inline_files = 0;
    for (std::size_t i = 0; i < messages.size(); ++i) inline_files += messages[i].files.size();
    platform::log_debug("chat[5/gemini]: ainvoke with %lu message(s), %lu inline file part(s)",
                        (unsigned long)messages.size(), (unsigned long)inline_files);
    auto emit_error = [&](std::string msg) {
        Event e; e.type = EventType::Error; e.content = std::move(msg);
        on_event(e);
    };

    if (opt_.mode == GeminiMode::AiStudio) {
        if (opt_.api_key.empty()) {
            emit_error("GOOGLE_API_KEY is required for Gemini AI Studio mode.");
            return false;
        }
    } else {
        if (opt_.access_token.empty()) {
            emit_error("access_token is required for Gemini Vertex mode "
                       "(get one via `gcloud auth print-access-token`).");
            return false;
        }
        if (opt_.gcp_project.empty() || opt_.gcp_location.empty()) {
            emit_error("gcp_project and gcp_location are required for Gemini Vertex mode.");
            return false;
        }
    }
    if (messages.empty()) {
        emit_error("Empty message.");
        return false;
    }

    const std::string url = build_url(opt_);
    const bool tools_enabled = !opt_.tools_json.empty() && static_cast<bool>(opt_.tool_executor);

    // The running conversation as raw JSON content turns; the tool loop appends
    // the model's functionCall turn and the functionResponse turn between rounds.
    std::vector<std::string> content_objs;
    for (const auto& m : messages) {
        // OpenAI-style "assistant" maps to Gemini's "model"; everything else is
        // "user" (Gemini doesn't accept "system" inside contents).
        const char* role = (m.role == "assistant" || m.role == "model")
                                ? "model" : "user";
        content_objs.push_back(m.files.empty()
            ? make_content_text(role, m.content)
            : make_content_with_files(role, m.content, m.files));
    }

    std::int64_t in_tok = 0, out_tok = 0, thought_tok = 0, total_tok = 0;

    int call_seq = 0;   // synthetic tool_id counter, carried across rounds

    for (int iter = 0; ; ++iter) {
        const std::string body = build_request_body(opt_, content_objs, system_prompt, tools_enabled);

        StreamContext stream;
        stream.min_chunk_size    = opt_.min_chunk_size;
        stream.on_event          = &on_event;
        stream.function_call_seq = call_seq;

        const RequestOutcome o = run_request(opt_, url, body, stream);
        call_seq = stream.function_call_seq;

        if (o.init_failed)   { emit_error("curl_easy_init failed"); return false; }
        if (stream.aborted)  return false;
        if (o.rc != CURLE_OK) {
            emit_error(std::string{"transport error: "} + curl_easy_strerror(o.rc));
            return false;
        }
        if (o.http_error) {
            emit_error(o.http_error_body.empty()
                ? std::string{"HTTP "} + std::to_string(o.http_code)
                : o.http_error_body);
            return false;
        }

        flush_text(stream);
        if (stream.aborted) return false;

        // Gemini reports a cumulative snapshot per request; sum the final value
        // of each round.
        in_tok      += stream.input_tokens;
        out_tok     += stream.output_tokens;
        thought_tok += stream.thought_tokens;
        total_tok   += stream.total_tokens;

        if (!tools_enabled || stream.calls.empty() || iter >= opt_.max_tool_iterations) {
            break;
        }

        // Replay the model's call turn, run each tool, and append the responses.
        content_objs.push_back(make_model_function_call_turn(stream.full_text, stream.calls));

        std::vector<std::string> results;
        results.reserve(stream.calls.size());
        for (std::size_t i = 0; i < stream.calls.size(); ++i) {
            const FunctionCall& c = stream.calls[i];
            const std::string result = opt_.tool_executor(c.name, c.args_json, user);
            results.push_back(result);

            Event det;
            det.type    = EventType::QueryDetail;
            det.content = result;
            det.tool_id = c.id;
            if (!on_event(det)) return false;   // client gone
        }
        content_objs.push_back(make_function_response_turn(stream.calls, results));
    }

    Event cs;
    cs.type                = EventType::CostStatistics;
    cs.cost.model          = opt_.model;
    cs.cost.input_tokens   = in_tok;
    cs.cost.output_tokens  = out_tok;
    cs.cost.thought_tokens = thought_tok;
    cs.cost.total_tokens   = total_tok;
    cs.cost.total_cost =
        (static_cast<double>(in_tok) * opt_.input_price +
         static_cast<double>(thought_tok + out_tok) * opt_.output_price)
        / 1e6;
    on_event(cs);

    return true;
}

}
}
