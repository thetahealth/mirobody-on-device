#include "transcode/parser.hpp"

#include "client/gcp_auth.hpp"    // Vertex: the shared token source
#include "client/http_client.hpp"
#include "config/config.hpp"
#include "platform/log.hpp"
#include "storage/sign.hpp"      // base64_encode
#include "transcode/image.hpp"   // Transcoder + per-provider Limits

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace mirobody { namespace file {

namespace {

// The instruction handed to every backend. Deliberately strict: we want the
// file's text verbatim, not a summary or commentary, so the cached result is
// usable as source material rather than a paraphrase.
const char* const kPrompt =
    "You are a document text-extraction engine. Transcribe ALL text from the "
    "attached file verbatim, preserving reading order. Output only the extracted "
    "text -- no commentary, no headings you add yourself, no markdown code "
    "fences. If the file contains no readable text, output nothing.";

// Largest cut point at or below `limit` that does not split a UTF-8 sequence:
// step back over continuation bytes (10xxxxxx) to the start of the character
// they belong to. Worth the care because extracted text is routinely CJK, where
// characters are three bytes and a blind byte cut lands mid-character two times
// in three -- the invalid tail would then ride into a JSON response body. Input
// that is not UTF-8 (no lead byte within a sequence's reach) is cut at `limit`.
std::size_t utf8_cut(const std::string& s, std::size_t limit) {
    if (limit >= s.size()) return s.size();
    std::size_t n = limit;
    for (int back = 0; back < 4 && n > 0; ++back) {
        if ((static_cast<unsigned char>(s[n]) & 0xC0) != 0x80) return n;
        --n;
    }
    return limit;
}

// Serialize a rapidjson value to a compact string.
std::string serialize(const rapidjson::Value& v) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    v.Accept(w);
    return std::string(buf.GetString(), buf.GetSize());
}

// A short, safe snippet of an error body for logging.
std::string snippet(const std::string& s) {
    return s.size() > 300 ? s.substr(0, 300) + "..." : s;
}

// Tighten a provider preset for OCR. The stock presets only guard against
// outright rejection (Qwen allows 10 MB / 6.5 MP), which leaves a large scan to
// pass through verbatim -- and a bulky PNG makes vision OCR crawl or time out.
// Capping pixels (=> fewer vision tokens => faster) and bytes forces such an
// image down the transcoder's re-encode path, which emits a compact JPEG that
// the models handle far better than a big PNG. Tunable via env.
image::Limits ocr_limits(image::Limits l, const Config& cfg) {
    const long long px = cfg.store.get_int("FILE_PARSER_MAX_PIXELS", 1600LL * 1600);   // ~2.5 MP
    const long long kb = cfg.store.get_int("FILE_PARSER_MAX_KB", 768);                 // 768 KB
    if (px > 0 && l.max_pixels > px) l.max_pixels = px;
    if (kb > 0 && l.max_bytes > static_cast<std::size_t>(kb) * 1024) {
        l.max_bytes = static_cast<std::size_t>(kb) * 1024;
    }
    return l;
}

// Conform a decodable raster image to the target model's input limits before we
// send it: resize within the pixel/byte budget (and re-encode), or pass through
// untouched when already compliant. This is what keeps an oversized upload from
// being rejected outright -- and, by capping the pixel count, keeps vision OCR
// from running long enough to time out. Inputs the codec stack can't decode
// (PDF, GIF, ...) pass through unchanged; a decode failure also degrades to the
// original bytes rather than failing the extraction. Updates `data`/`mime`.
void conform_image(const image::Limits& limits, const std::string& filename,
                   std::string& data, std::string& mime) {
    if (image::Transcoder::detect_format(data) == image::Format::Unknown) {
        platform::log_info("file parser: '%s' is not a decodable raster image; "
                           "sending %zu bytes as-is", filename.c_str(), data.size());
        return;
    }
    try {
        const image::Transcoded out = image::Transcoder(limits).transcode(data);
        platform::log_info("file parser: image '%s' %s -> %s %dx%d (%zu -> %zu bytes)",
                           filename.c_str(), out.changed ? "conformed" : "within limits",
                           out.content_type.c_str(), out.width, out.height,
                           data.size(), out.bytes.size());
        data = out.bytes;
        mime = out.content_type;
    } catch (const image::ImageError& e) {
        platform::log_warn("file parser: image transcode failed for '%s' (%s); sending original",
                           filename.c_str(), e.what());
    }
}

//------------------------------------------------------------------------------
// Gemini (Google AI Studio generateContent)

// Google's generateContent, on either surface. The wire format is identical --
// same request body, same response shape -- so only the URL and the credential
// header differ, and both are decided once at construction:
//
//   AI Studio: .../v1beta/models/{model}:generateContent
//              x-goog-api-key: {key}
//   Vertex   : {host}/v1/projects/{p}/locations/{l}/publishers/google/models/
//              {model}:generateContent
//              Authorization: Bearer {token}, resolved per request
//
// Which one is in play matters beyond the endpoint: uploads are the most
// sensitive thing this server sends anywhere -- the whole file, not a summary --
// and AI Studio is not covered by Google's BAA. A deployment that put its chat on
// Vertex for that reason would not expect its documents to leave by the other
// door, so the parser follows the same switch rather than carrying its own.
class GeminiParser : public Parser {
public:
    // `auth_header` returns the complete header line to send, resolved at call
    // time (a Vertex token may have been rotated since construction). Returning
    // "" means the credential is gone, and the extraction is skipped.
    GeminiParser(std::string url, std::function<std::string()> auth_header,
                 std::string surface, int timeout_ms, image::Limits limits)
        : url_(std::move(url)), auth_header_(std::move(auth_header)),
          surface_(std::move(surface)), timeout_ms_(timeout_ms), limits_(limits) {}

    const char* name() const override { return "gemini"; }

    std::string extract_text(const std::string& bytes, const std::string& mime_type,
                             const std::string& filename) override {
        const std::string auth = auth_header_ ? auth_header_() : std::string();
        if (auth.empty()) {
            platform::log_warn("file parser[gemini/%s]: no credential; skipping '%s'",
                               surface_.c_str(), filename.c_str());
            return std::string();
        }

        // { contents:[{ role:"user", parts:[ {text}, {inline_data:{mime_type,data}} ] }],
        //   generationConfig:{ temperature:0 } }
        rapidjson::Document d;
        d.SetObject();
        rapidjson::Document::AllocatorType& a = d.GetAllocator();

        std::string data = bytes;
        std::string mime = mime_type.empty() ? "application/octet-stream" : mime_type;
        conform_image(limits_, filename, data, mime);
        const std::string b64 = storage::base64_encode(data);

        rapidjson::Value text_part(rapidjson::kObjectType);
        text_part.AddMember("text", rapidjson::StringRef(kPrompt), a);

        rapidjson::Value inline_data(rapidjson::kObjectType);
        inline_data.AddMember("mime_type",
                              rapidjson::Value(mime.c_str(),
                                  static_cast<rapidjson::SizeType>(mime.size()), a), a);
        inline_data.AddMember("data",
                              rapidjson::Value(b64.c_str(),
                                  static_cast<rapidjson::SizeType>(b64.size()), a), a);
        rapidjson::Value file_part(rapidjson::kObjectType);
        file_part.AddMember("inline_data", inline_data, a);

        rapidjson::Value parts(rapidjson::kArrayType);
        parts.PushBack(text_part, a);
        parts.PushBack(file_part, a);

        rapidjson::Value content(rapidjson::kObjectType);
        content.AddMember("role", "user", a);
        content.AddMember("parts", parts, a);

        rapidjson::Value contents(rapidjson::kArrayType);
        contents.PushBack(content, a);
        d.AddMember("contents", contents, a);

        rapidjson::Value gen(rapidjson::kObjectType);
        gen.AddMember("temperature", 0.0, a);
        d.AddMember("generationConfig", gen, a);

        client::HttpClient http;
        client::HttpRequest req;
        req.url = url_;
        // The credential as a header, never in the URL: a URL is written down by
        // everything on the request's path (proxy logs, curl traces), a header is
        // not. Same form the embedding and chat clients use.
        req.headers.push_back(auth);
        req.body = serialize(d);
        req.request_timeout_ms = timeout_ms_;
        const client::HttpResponse resp = http.post(req);
        if (resp.status != 200) {
            platform::log_error("file parser[gemini/%s]: HTTP %ld for '%s': %s",
                                surface_.c_str(), resp.status, filename.c_str(),
                                snippet(resp.body).c_str());
            return std::string();
        }
        const std::string text = cap_text(parse(resp.body));
        platform::log_info("file parser[gemini/%s]: extracted %zu chars from '%s'",
                           surface_.c_str(), text.size(), filename.c_str());
        return text;
    }

private:
    // candidates[0].content.parts[*].text, concatenated.
    static std::string parse(const std::string& body) {
        rapidjson::Document r;
        if (r.Parse(body.c_str()).HasParseError() || !r.IsObject()) return std::string();
        rapidjson::Value::ConstMemberIterator c = r.FindMember("candidates");
        if (c == r.MemberEnd() || !c->value.IsArray() || c->value.Empty()) return std::string();
        const rapidjson::Value& cand0 = c->value[0];
        if (!cand0.IsObject()) return std::string();
        rapidjson::Value::ConstMemberIterator cont = cand0.FindMember("content");
        if (cont == cand0.MemberEnd() || !cont->value.IsObject()) return std::string();
        rapidjson::Value::ConstMemberIterator parts = cont->value.FindMember("parts");
        if (parts == cont->value.MemberEnd() || !parts->value.IsArray()) return std::string();
        std::string out;
        for (rapidjson::SizeType i = 0; i < parts->value.Size(); ++i) {
            rapidjson::Value::ConstMemberIterator t = parts->value[i].FindMember("text");
            if (t != parts->value[i].MemberEnd() && t->value.IsString()) {
                out.append(t->value.GetString(), t->value.GetStringLength());
            }
        }
        return out;
    }

    std::string                   url_;
    std::function<std::string()>  auth_header_;
    std::string                   surface_;   // "ai-studio" / "vertex", for the logs
    int                           timeout_ms_;
    image::Limits                 limits_;
};

//------------------------------------------------------------------------------
// Qwen (DashScope OpenAI-compatible chat/completions, multimodal content)

class QwenParser : public Parser {
public:
    QwenParser(std::string api_key, std::string base_url, std::string model,
               int timeout_ms, image::Limits limits)
        : api_key_(std::move(api_key)), base_url_(std::move(base_url)),
          model_(std::move(model)), timeout_ms_(timeout_ms), limits_(limits) {}

    const char* name() const override { return "qwen"; }

    std::string extract_text(const std::string& bytes, const std::string& mime_type,
                             const std::string& filename) override {
        if (api_key_.empty()) {
            platform::log_warn("file parser[qwen]: no API key; skipping '%s'",
                               filename.c_str());
            return std::string();
        }

        // { model, messages:[{ role:"user", content:[ {type:"text",text},
        //   {type:"image_url",image_url:{url:"data:<mime>;base64,<b64>"}} ] }],
        //   stream:false }
        rapidjson::Document d;
        d.SetObject();
        rapidjson::Document::AllocatorType& a = d.GetAllocator();
        d.AddMember("model",
                    rapidjson::Value(model_.c_str(),
                        static_cast<rapidjson::SizeType>(model_.size()), a), a);

        std::string data = bytes;
        std::string mime = mime_type.empty() ? "application/octet-stream" : mime_type;
        conform_image(limits_, filename, data, mime);
        const std::string data_url = "data:" + mime + ";base64," + storage::base64_encode(data);

        rapidjson::Value text_part(rapidjson::kObjectType);
        text_part.AddMember("type", "text", a);
        text_part.AddMember("text", rapidjson::StringRef(kPrompt), a);

        rapidjson::Value image_url(rapidjson::kObjectType);
        image_url.AddMember("url",
                            rapidjson::Value(data_url.c_str(),
                                static_cast<rapidjson::SizeType>(data_url.size()), a), a);
        rapidjson::Value image_part(rapidjson::kObjectType);
        image_part.AddMember("type", "image_url", a);
        image_part.AddMember("image_url", image_url, a);

        rapidjson::Value content(rapidjson::kArrayType);
        content.PushBack(text_part, a);
        content.PushBack(image_part, a);

        rapidjson::Value msg(rapidjson::kObjectType);
        msg.AddMember("role", "user", a);
        msg.AddMember("content", content, a);
        rapidjson::Value messages(rapidjson::kArrayType);
        messages.PushBack(msg, a);
        d.AddMember("messages", messages, a);
        d.AddMember("stream", false, a);

        client::HttpClient http;
        client::HttpRequest req;
        req.url = base_url_ + "/chat/completions";
        req.body = serialize(d);
        req.headers.push_back("Authorization: Bearer " + api_key_);
        req.request_timeout_ms = timeout_ms_;
        const client::HttpResponse resp = http.post(req);
        if (resp.status != 200) {
            platform::log_error("file parser[qwen]: HTTP %ld for '%s': %s",
                                resp.status, filename.c_str(), snippet(resp.body).c_str());
            return std::string();
        }
        const std::string text = cap_text(parse(resp.body));
        platform::log_info("file parser[qwen]: extracted %zu chars from '%s'",
                           text.size(), filename.c_str());
        return text;
    }

private:
    // choices[0].message.content (a string for these requests).
    static std::string parse(const std::string& body) {
        rapidjson::Document r;
        if (r.Parse(body.c_str()).HasParseError() || !r.IsObject()) return std::string();
        rapidjson::Value::ConstMemberIterator ch = r.FindMember("choices");
        if (ch == r.MemberEnd() || !ch->value.IsArray() || ch->value.Empty()) return std::string();
        const rapidjson::Value& c0 = ch->value[0];
        if (!c0.IsObject()) return std::string();
        rapidjson::Value::ConstMemberIterator m = c0.FindMember("message");
        if (m == c0.MemberEnd() || !m->value.IsObject()) return std::string();
        rapidjson::Value::ConstMemberIterator ct = m->value.FindMember("content");
        if (ct != m->value.MemberEnd() && ct->value.IsString()) {
            return std::string(ct->value.GetString(), ct->value.GetStringLength());
        }
        return std::string();
    }

    std::string   api_key_, base_url_, model_;
    int           timeout_ms_;
    image::Limits limits_;
};

// Lowercase a short config token.
std::string lower(std::string s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = static_cast<char>(s[i] - 'A' + 'a');
    }
    return s;
}

// Announce the backend the process will actually use. This is the only place an
// operator can see that extraction is on and what it will call: everything
// downstream is best-effort and silent by design -- a disabled parser and one
// that fails on every upload both simply produce no text, and the only visible
// trace either way is a missing <key>.trans object. Logged once, at startup.
void log_selection(const char* backend, const std::string& model, bool has_credential) {
    if (has_credential) {
        platform::log_info("file parser: %s (model=%s)", backend, model.c_str());
        return;
    }
    platform::log_warn("file parser: %s (model=%s) has NO credential configured; every "
                       "upload will skip extraction and keep no text", backend, model.c_str());
}

}   // namespace

//------------------------------------------------------------------------------

std::string cap_text(std::string s) {
    if (s.size() <= kMaxTextBytes) return s;
    s.resize(utf8_cut(s, kMaxTextBytes));
    s += "\n...[truncated]";
    return s;
}

//------------------------------------------------------------------------------

std::unique_ptr<Parser> make_parser(const Config& cfg) {
    const std::string sel = lower(cfg.store.get_str("FILE_PARSER", "gemini"));

    if (sel.empty() || sel == "none" || sel == "off" || sel == "disabled") {
        // Worth a line even though it is a deliberate choice: with no extracted
        // text, an upload is only readable by a model that can read the file
        // itself, in the turn it arrived on. The read_file tool then has nothing
        // but a URL to hand back, on this turn and every later one.
        platform::log_info("file parser: disabled (FILE_PARSER='%s'); uploads keep no "
                           "extracted text", sel.empty() ? "" : sel.c_str());
        return std::unique_ptr<Parser>();
    }

    // Vision OCR over a large image can be slow; allow a generous, configurable
    // request timeout (default 5 min).
    const int timeout = static_cast<int>(cfg.store.get_int("FILE_PARSER_TIMEOUT_MS", 300000));

    if (sel == "gemini") {
        // Same switch, same two keys as the chat lane (res/agents/baseline.cpp):
        // a project AND a location mean Vertex. One Vertex configuration decides
        // where EVERYTHING Google-bound goes -- which matters most here, since
        // what this lane sends is the uploaded file itself, and AI Studio is not
        // covered by Google's BAA.
        const std::string project  = cfg.store.get_str("GOOGLE_CLOUD_PROJECT");
        const std::string location = cfg.store.get_str("GOOGLE_CLOUD_LOCATION");

        if (!project.empty() && !location.empty()) {
            // Vertex serves gemini-3.5-flash; 3.6 publishes only the `global`
            // location, so a region-pinned project cannot reach it (the chat
            // lane's model table says the same thing).
            const std::string model = cfg.store.get_str("FILE_PARSER_GEMINI_MODEL",
                                                        "gemini-3.5-flash");
            // Region, us / eu multi-region and "global" each spell the host
            // differently; gcp::vertex_host is the one place that knows how.
            const std::string host = cfg.store.get_str("GEMINI_VERTEX_BASE_URL",
                                                       gcp::vertex_host(location));
            const std::string url = host + "/v1/projects/" + project + "/locations/" + location
                                  + "/publishers/google/models/" + model + ":generateContent";

            // Shared resolution order (token file, inline token, ADC), resolved
            // per request so a rotated token lands without a restart.
            std::shared_ptr<gcp::TokenSource> tokens = std::make_shared<gcp::TokenSource>(
                cfg.store.get_str("GCP_ACCESS_TOKEN_FILE"),
                cfg.store.get_str("GCP_ACCESS_TOKEN"));
            platform::log_info("file parser: gemini via Vertex AI (model=%s, project=%s, "
                               "location=%s, token=%s)",
                               model.c_str(), project.c_str(), location.c_str(),
                               tokens->describe());
            return std::unique_ptr<Parser>(new GeminiParser(
                url,
                [tokens]() {
                    const std::string t = tokens->token();
                    return t.empty() ? std::string() : "Authorization: Bearer " + t;
                },
                "vertex", timeout, ocr_limits(image::GeminiLimits, cfg)));
        }

        const std::string key   = cfg.store.get_str("GOOGLE_API_KEY", cfg.gemini.api_key);
        const std::string model = cfg.store.get_str("FILE_PARSER_GEMINI_MODEL", "gemini-3.6-flash");
        log_selection("gemini via AI Studio", model, !key.empty());
        const std::string url = cfg.gemini.base_url + "/v1beta/models/" + model
                              + ":generateContent";
        return std::unique_ptr<Parser>(new GeminiParser(
            url,
            [key]() { return key.empty() ? std::string() : "x-goog-api-key: " + key; },
            "ai-studio", timeout, ocr_limits(image::GeminiLimits, cfg)));
    }

    if (sel == "qwen") {
        const std::string key = cfg.dashscope.api_key.empty()
            ? cfg.store.get_str("DASHSCOPE_API_KEY") : cfg.dashscope.api_key;
        // Must be a vision-capable model (it receives an image). Override per
        // account via FILE_PARSER_QWEN_MODEL.
        const std::string model = cfg.store.get_str("FILE_PARSER_QWEN_MODEL", "qwen3.7-plus");
        log_selection("qwen", model, !key.empty());
        return std::unique_ptr<Parser>(new QwenParser(
            key, cfg.dashscope.base_url, model, timeout, ocr_limits(image::QwenLimits, cfg)));
    }

    platform::log_warn("file parser: unknown FILE_PARSER='%s'; extraction disabled",
                       sel.c_str());
    return std::unique_ptr<Parser>();
}

}}
