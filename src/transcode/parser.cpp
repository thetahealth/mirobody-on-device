#include "transcode/parser.hpp"

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "platform/log.hpp"
#include "storage/sign.hpp"      // base64_encode
#include "transcode/image.hpp"   // Transcoder + per-provider Limits

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <string>

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

// Upper bound on cached extracted text, so one huge document can't blow up the
// per-user cache entry. Truncated past this with an explicit marker.
const std::size_t kMaxChars = 100000;

std::string cap(std::string s) {
    if (s.size() > kMaxChars) {
        s.resize(kMaxChars);
        s += "\n...[truncated]";
    }
    return s;
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

class GeminiParser : public Parser {
public:
    GeminiParser(std::string api_key, std::string base_url, std::string model,
                 int timeout_ms, image::Limits limits)
        : api_key_(std::move(api_key)), base_url_(std::move(base_url)),
          model_(std::move(model)), timeout_ms_(timeout_ms), limits_(limits) {}

    const char* name() const override { return "gemini"; }

    std::string extract_text(const std::string& bytes, const std::string& mime_type,
                             const std::string& filename) override {
        if (api_key_.empty()) {
            platform::log_warn("file parser[gemini]: no API key; skipping '%s'",
                               filename.c_str());
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
        req.url = base_url_ + "/v1beta/models/" + model_ + ":generateContent?key=" + api_key_;
        req.body = serialize(d);
        req.request_timeout_ms = timeout_ms_;
        const client::HttpResponse resp = http.post(req);
        if (resp.status != 200) {
            platform::log_error("file parser[gemini]: HTTP %ld for '%s': %s",
                                resp.status, filename.c_str(), snippet(resp.body).c_str());
            return std::string();
        }
        const std::string text = cap(parse(resp.body));
        platform::log_info("file parser[gemini]: extracted %zu chars from '%s'",
                           text.size(), filename.c_str());
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

    std::string   api_key_, base_url_, model_;
    int           timeout_ms_;
    image::Limits limits_;
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
        const std::string text = cap(parse(resp.body));
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

}   // namespace

//------------------------------------------------------------------------------

std::unique_ptr<Parser> make_parser(const Config& cfg) {
    const std::string sel = lower(cfg.store.get_str("FILE_PARSER", "gemini"));

    if (sel.empty() || sel == "none" || sel == "off" || sel == "disabled") {
        return std::unique_ptr<Parser>();
    }

    // Vision OCR over a large image can be slow; allow a generous, configurable
    // request timeout (default 5 min).
    const int timeout = static_cast<int>(cfg.store.get_int("FILE_PARSER_TIMEOUT_MS", 300000));

    if (sel == "gemini") {
        const std::string key   = cfg.store.get_str("GOOGLE_API_KEY", cfg.gemini.api_key);
        const std::string model = cfg.store.get_str("FILE_PARSER_GEMINI_MODEL", "gemini-3.5-flash");
        return std::unique_ptr<Parser>(new GeminiParser(
            key, cfg.gemini.base_url, model, timeout, ocr_limits(image::GeminiLimits, cfg)));
    }

    if (sel == "qwen") {
        const std::string key = cfg.dashscope.api_key.empty()
            ? cfg.store.get_str("DASHSCOPE_API_KEY") : cfg.dashscope.api_key;
        // Must be a vision-capable model (it receives an image). Override per
        // account via FILE_PARSER_QWEN_MODEL.
        const std::string model = cfg.store.get_str("FILE_PARSER_QWEN_MODEL", "qwen3.7-plus");
        return std::unique_ptr<Parser>(new QwenParser(
            key, cfg.dashscope.base_url, model, timeout, ocr_limits(image::QwenLimits, cfg)));
    }

    platform::log_warn("file parser: unknown FILE_PARSER='%s'; extraction disabled",
                       sel.c_str());
    return std::unique_ptr<Parser>();
}

}}
