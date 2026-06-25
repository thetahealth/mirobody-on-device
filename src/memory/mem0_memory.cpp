#include "memory/mem0_memory.hpp"

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
// Mem0 (mem0.ai) REST contract, over `${MEM0_BASE_URL}` (default the hosted
// platform) with header `Authorization: Token ${MEM0_API_KEY}`:
//
//   POST /v1/memories/          {messages:[{role,content}], user_id, metadata}
//                               -> [ {id, memory, event}, ... ]  (added events)
//   POST /v1/memories/search/   {query, user_id, limit}
//                               -> {results:[ {id, memory, score, metadata}, ... ]}
//   DELETE /v1/memories/{id}/                                    -> 2xx
//
// Mem0 mints opaque string ids and extracts/condenses facts itself, so this
// adapter does not parse a numeric id back: remember() returns a positive
// sentinel to signal success (see memory.hpp), and recall() leaves Record.id 0.
// Field/endpoint names track Mem0's documented API as of early 2026 -- verify
// against current docs if Mem0 revises them.
//------------------------------------------------------------------------------

const char* kDefaultBase = "https://api.mem0.ai";
const std::int64_t kStoredSentinel = 1;   // remote string-id store: success, no numeric id

std::string trim_trailing_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string member_str(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return std::string();
    const rapidjson::Value& m = v[key];
    return m.IsString() ? std::string(m.GetString(), m.GetStringLength()) : std::string();
}

double member_num(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return 0.0;
    const rapidjson::Value& m = v[key];
    return m.IsNumber() ? m.GetDouble() : 0.0;
}

//------------------------------------------------------------------------------

class Mem0Memory : public Memory {
public:
    Mem0Memory(std::string base, std::string key, int default_top_k,
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
        w.Key("messages");
        w.StartArray();
        w.StartObject();
        w.Key("role");    w.String("user");
        w.Key("content"); w.String(in.text.c_str(), static_cast<rapidjson::SizeType>(in.text.size()));
        w.EndObject();
        w.EndArray();
        w.Key("user_id"); w.String(user_key(in.user_id).c_str());
        w.Key("metadata");
        w.StartObject();
        w.Key("kind"); w.String(in.kind.c_str(), static_cast<rapidjson::SizeType>(in.kind.size()));
        if (!in.session_id.empty()) {
            w.Key("session_id");
            w.String(in.session_id.c_str(), static_cast<rapidjson::SizeType>(in.session_id.size()));
        }
        w.EndObject();
        w.EndObject();

        const client::HttpResponse res = post(base_ + "/v1/memories/",
                                              std::string(sb.GetString(), sb.GetSize()));
        if (res.status < 200 || res.status >= 300) {
            if (err) *err = "memory: mem0 store failed (HTTP " + std::to_string(res.status) + ")";
            return 0;
        }
        return kStoredSentinel;
    }

    std::vector<Record> recall(std::int64_t user_id, const std::string& query,
                               int top_k, std::string* err) override {
        std::vector<Record> out;
        if (user_id <= 0 || query.empty()) return out;
        if (top_k <= 0) top_k = default_top_k_;

        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject();
        w.Key("query");   w.String(query.c_str(), static_cast<rapidjson::SizeType>(query.size()));
        w.Key("user_id"); w.String(user_key(user_id).c_str());
        w.Key("limit");   w.Int(top_k);
        w.EndObject();

        const client::HttpResponse res = post(base_ + "/v1/memories/search/",
                                              std::string(sb.GetString(), sb.GetSize()));
        if (res.status < 200 || res.status >= 300) {
            if (err) *err = "memory: mem0 recall failed (HTTP " + std::to_string(res.status) + ")";
            return out;
        }
        rapidjson::Document d;
        if (d.Parse(res.body.c_str()).HasParseError()) {
            if (err) *err = "memory: mem0 recall returned malformed JSON";
            return out;
        }
        // Mem0 returns either {results:[...]} or a bare array depending on
        // version; accept both.
        const rapidjson::Value* arr = nullptr;
        if (d.IsObject() && d.HasMember("results") && d["results"].IsArray()) arr = &d["results"];
        else if (d.IsArray()) arr = &d;
        if (!arr) return out;

        out.reserve(arr->Size());
        for (rapidjson::SizeType i = 0; i < arr->Size(); ++i) {
            const rapidjson::Value& e = (*arr)[i];
            Record rec;
            rec.text  = member_str(e, "memory");
            if (rec.text.empty()) rec.text = member_str(e, "text");
            rec.score = member_num(e, "score");
            if (e.IsObject() && e.HasMember("metadata"))
                rec.kind = member_str(e["metadata"], "kind");
            out.push_back(rec);
        }
        return out;
    }

    bool forget(std::int64_t user_id, std::int64_t /*id*/, std::string* err) override {
        // Mem0 keys memories by opaque string id, which this int64-keyed
        // interface cannot address; deletion is left to the Mem0 console / SDK.
        if (user_id <= 0) return false;
        if (err) *err = "memory: forget is not supported by the mem0 backend";
        return false;
    }

private:
    // Mem0 user ids are strings; namespace ours so they don't collide with any
    // other producer writing to the same Mem0 project.
    std::string user_key(std::int64_t user_id) const {
        return "mirobody:" + std::to_string(user_id);
    }

    client::HttpResponse post(const std::string& url, const std::string& body) const {
        client::HttpRequest req;
        req.url     = url;
        req.body    = body;
        req.headers = std::vector<std::string>();
        if (!key_.empty()) req.headers.push_back("Authorization: Token " + key_);
        req.connect_timeout_ms = connect_ms_;
        req.request_timeout_ms = request_ms_;
        client::HttpClient http;
        return http.post(req);
    }

    std::string base_;
    std::string key_;
    int         default_top_k_;
    int         connect_ms_;
    int         request_ms_;
};

}   // namespace

//------------------------------------------------------------------------------

std::unique_ptr<Memory> make_mem0_memory(const Config& cfg) {
    if (cfg.memory.api_key.empty()) {
        platform::log_warn("memory: MEMORY_PROVIDER=mem0 but no API key set "
                           "(MEM0_API_KEY / MEMORY_API_KEY); memory disabled");
        return std::unique_ptr<Memory>();
    }
    const std::string base = cfg.memory.base_url.empty() ? kDefaultBase : cfg.memory.base_url;
    return std::unique_ptr<Memory>(new Mem0Memory(
        base, cfg.memory.api_key, cfg.memory.top_k,
        cfg.connect_timeout_ms, cfg.request_timeout_ms));
}

}}
