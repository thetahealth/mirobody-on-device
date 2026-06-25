#include "memory/remote_memory.hpp"

#include "client/http_client.hpp"
#include "config/config.hpp"
#include "platform/log.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace memory {

namespace {

//------------------------------------------------------------------------------
// RemoteMemory speaks a small EverOS-compatible JSON/REST contract (EverOS:
// https://github.com/EverMind-AI/EverOS) over
// `${EVEROS_BASE_URL}` with a Bearer `${EVEROS_API_KEY}`:
//
//   POST  /memories          {user_id, text, kind, session_id} -> {"id": <int>}
//   POST  /memories/search   {user_id, query, top_k}           -> {"results":[
//                              {id, text, kind, session_id, created_at, score}, ...]}
//   DELETE /memories/{id}?user_id=<id>                          -> 2xx (deleted)
//
// The mirobody side stays backend-neutral: an adapter in front of a different
// memory service only has to satisfy these four shapes.
//------------------------------------------------------------------------------

std::string trim_trailing_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// JSON string member -> std::string ("" when absent / not a string).
std::string member_str(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return std::string();
    const rapidjson::Value& m = v[key];
    return m.IsString() ? std::string(m.GetString(), m.GetStringLength()) : std::string();
}

std::int64_t member_int(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return 0;
    const rapidjson::Value& m = v[key];
    if (m.IsInt64()) return m.GetInt64();
    if (m.IsInt())   return m.GetInt();
    return 0;
}

double member_num(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return 0.0;
    const rapidjson::Value& m = v[key];
    return m.IsNumber() ? m.GetDouble() : 0.0;
}

//------------------------------------------------------------------------------

class RemoteMemory : public Memory {
public:
    RemoteMemory(std::string base, std::string key, int default_top_k,
                 int connect_ms, int request_ms)
        : base_(trim_trailing_slash(std::move(base)))
        , key_(std::move(key))
        , default_top_k_(default_top_k > 0 ? default_top_k : 5)
        , connect_ms_(connect_ms)
        , request_ms_(request_ms) {}

    std::int64_t remember(const RememberInput& in, std::string* err) override {
        if (in.user_id <= 0) {
            if (err) *err = "memory: a logged-in user is required";
            return 0;
        }
        if (in.text.empty()) return 0;

        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject();
        w.Key("user_id");    w.Int64(in.user_id);
        w.Key("text");       w.String(in.text.c_str(), static_cast<rapidjson::SizeType>(in.text.size()));
        w.Key("kind");       w.String(in.kind.c_str(),  static_cast<rapidjson::SizeType>(in.kind.size()));
        w.Key("session_id"); w.String(in.session_id.c_str(), static_cast<rapidjson::SizeType>(in.session_id.size()));
        w.EndObject();

        client::HttpRequest req;
        req.url     = base_ + "/memories";
        req.body    = std::string(sb.GetString(), sb.GetSize());
        req.headers = auth_headers();
        req.connect_timeout_ms = connect_ms_;
        req.request_timeout_ms = request_ms_;

        client::HttpClient http;
        const client::HttpResponse res = http.post(req);
        if (res.status < 200 || res.status >= 300) {
            if (err) *err = "memory: remote store failed (HTTP " + std::to_string(res.status) + ")";
            return 0;
        }
        rapidjson::Document d;
        if (d.Parse(res.body.c_str()).HasParseError()) {
            if (err) *err = "memory: remote store returned malformed JSON";
            return 0;
        }
        return member_int(d, "id");
    }

    std::vector<Record> recall(std::int64_t user_id, const std::string& query,
                               int top_k, std::string* err) override {
        std::vector<Record> out;
        if (user_id <= 0 || query.empty()) return out;
        if (top_k <= 0) top_k = default_top_k_;

        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject();
        w.Key("user_id"); w.Int64(user_id);
        w.Key("query");   w.String(query.c_str(), static_cast<rapidjson::SizeType>(query.size()));
        w.Key("top_k");   w.Int(top_k);
        w.EndObject();

        client::HttpRequest req;
        req.url     = base_ + "/memories/search";
        req.body    = std::string(sb.GetString(), sb.GetSize());
        req.headers = auth_headers();
        req.connect_timeout_ms = connect_ms_;
        req.request_timeout_ms = request_ms_;

        client::HttpClient http;
        const client::HttpResponse res = http.post(req);
        if (res.status < 200 || res.status >= 300) {
            if (err) *err = "memory: remote recall failed (HTTP " + std::to_string(res.status) + ")";
            return out;
        }
        rapidjson::Document d;
        if (d.Parse(res.body.c_str()).HasParseError() || !d.IsObject() ||
            !d.HasMember("results") || !d["results"].IsArray()) {
            if (err) *err = "memory: remote recall returned malformed JSON";
            return out;
        }
        const rapidjson::Value& arr = d["results"];
        out.reserve(arr.Size());
        for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
            const rapidjson::Value& e = arr[i];
            Record rec;
            rec.id         = member_int(e, "id");
            rec.text       = member_str(e, "text");
            rec.kind       = member_str(e, "kind");
            rec.session_id = member_str(e, "session_id");
            rec.created_at = member_int(e, "created_at");
            rec.score      = member_num(e, "score");
            out.push_back(rec);
        }
        return out;
    }

    bool forget(std::int64_t user_id, std::int64_t id, std::string* err) override {
        if (user_id <= 0 || id <= 0) return false;
        client::HttpRequest req;
        req.url = base_ + "/memories/" + std::to_string(id) +
                  "?user_id=" + std::to_string(user_id);
        req.headers = auth_headers();
        req.connect_timeout_ms = connect_ms_;
        req.request_timeout_ms = request_ms_;

        client::HttpClient http;
        const client::HttpResponse res = http.request("DELETE", req);
        if (res.status < 200 || res.status >= 300) {
            if (err) *err = "memory: remote delete failed (HTTP " + std::to_string(res.status) + ")";
            return false;
        }
        return true;
    }

private:
    std::vector<std::string> auth_headers() const {
        std::vector<std::string> h;
        if (!key_.empty()) h.push_back("Authorization: Bearer " + key_);
        return h;
    }

    std::string base_;
    std::string key_;
    int         default_top_k_;
    int         connect_ms_;
    int         request_ms_;
};

}   // namespace

//------------------------------------------------------------------------------

std::unique_ptr<Memory> make_remote_memory(const Config& cfg) {
    if (cfg.memory.base_url.empty()) return std::unique_ptr<Memory>();
    return std::unique_ptr<Memory>(new RemoteMemory(
        cfg.memory.base_url, cfg.memory.api_key, cfg.memory.top_k,
        cfg.connect_timeout_ms, cfg.request_timeout_ms));
}

}}
