#include "memory/zep_memory.hpp"

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
// Zep (getzep.com) REST contract, over `${ZEP_BASE_URL}` (default the hosted
// cloud) with header `Authorization: Api-Key ${ZEP_API_KEY}`:
//
//   POST /api/v2/graph         {user_id, type:"text", data:<text>}     -> 2xx
//   POST /api/v2/graph/search  {user_id, query, scope:"edges", limit}
//                              -> {edges:[ {fact, score, ...}, ... ]}
//
// Zep models memory as a per-user temporal knowledge graph and mints opaque
// string ids, so this adapter does not parse a numeric id: remember() returns a
// positive sentinel to signal success (see memory.hpp), and recall() reads the
// extracted `fact` text off each returned edge (id left 0). Field/endpoint
// names track Zep's documented API as of early 2026 -- verify against current
// docs if Zep revises them.
//------------------------------------------------------------------------------

const char* kDefaultBase = "https://api.getzep.com";
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

class ZepMemory : public Memory {
public:
    ZepMemory(std::string base, std::string key, int default_top_k,
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
        w.Key("user_id"); w.String(user_key(in.user_id).c_str());
        w.Key("type");    w.String("text");
        w.Key("data");    w.String(in.text.c_str(), static_cast<rapidjson::SizeType>(in.text.size()));
        w.EndObject();

        const client::HttpResponse res = post(base_ + "/api/v2/graph",
                                              std::string(sb.GetString(), sb.GetSize()));
        if (res.status < 200 || res.status >= 300) {
            if (err) *err = "memory: zep store failed (HTTP " + std::to_string(res.status) + ")";
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
        w.Key("user_id"); w.String(user_key(user_id).c_str());
        w.Key("query");   w.String(query.c_str(), static_cast<rapidjson::SizeType>(query.size()));
        w.Key("scope");   w.String("edges");
        w.Key("limit");   w.Int(top_k);
        w.EndObject();

        const client::HttpResponse res = post(base_ + "/api/v2/graph/search",
                                              std::string(sb.GetString(), sb.GetSize()));
        if (res.status < 200 || res.status >= 300) {
            if (err) *err = "memory: zep recall failed (HTTP " + std::to_string(res.status) + ")";
            return out;
        }
        rapidjson::Document d;
        if (d.Parse(res.body.c_str()).HasParseError() || !d.IsObject() ||
            !d.HasMember("edges") || !d["edges"].IsArray()) {
            if (err) *err = "memory: zep recall returned malformed JSON";
            return out;
        }
        const rapidjson::Value& edges = d["edges"];
        out.reserve(edges.Size());
        for (rapidjson::SizeType i = 0; i < edges.Size(); ++i) {
            const rapidjson::Value& e = edges[i];
            Record rec;
            rec.text  = member_str(e, "fact");
            rec.kind  = "fact";
            rec.score = member_num(e, "score");
            if (!rec.text.empty()) out.push_back(rec);
        }
        return out;
    }

    bool forget(std::int64_t user_id, std::int64_t /*id*/, std::string* err) override {
        // Zep edges/nodes are keyed by opaque uuid, which this int64-keyed
        // interface cannot address; deletion is left to the Zep console / SDK.
        if (user_id <= 0) return false;
        if (err) *err = "memory: forget is not supported by the zep backend";
        return false;
    }

private:
    std::string user_key(std::int64_t user_id) const {
        return "mirobody:" + std::to_string(user_id);
    }

    client::HttpResponse post(const std::string& url, const std::string& body) const {
        client::HttpRequest req;
        req.url     = url;
        req.body    = body;
        req.headers = std::vector<std::string>();
        if (!key_.empty()) req.headers.push_back("Authorization: Api-Key " + key_);
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

std::unique_ptr<Memory> make_zep_memory(const Config& cfg) {
    if (cfg.memory.api_key.empty()) {
        platform::log_warn("memory: MEMORY_PROVIDER=zep but no API key set "
                           "(ZEP_API_KEY / MEMORY_API_KEY); memory disabled");
        return std::unique_ptr<Memory>();
    }
    const std::string base = cfg.memory.base_url.empty() ? kDefaultBase : cfg.memory.base_url;
    return std::unique_ptr<Memory>(new ZepMemory(
        base, cfg.memory.api_key, cfg.memory.top_k,
        cfg.connect_timeout_ms, cfg.request_timeout_ms));
}

}}
