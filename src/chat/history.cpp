#include "chat/history.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace mirobody { namespace chat {

namespace {

std::string key_for(std::int64_t user_id, const std::string& session_id) {
    return "mirobody:chat:history:v1:" + std::to_string(user_id) + ":" + session_id;
}

// One {role, content} row of the conversation list.
std::string serialize_one(const llm::ChatMessage& m) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("role");
    w.String(m.role.c_str(), static_cast<rapidjson::SizeType>(m.role.size()));
    w.Key("content");
    w.String(m.content.c_str(), static_cast<rapidjson::SizeType>(m.content.size()));
    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize());
}

// Decode one row. False when malformed or missing a role.
bool parse_one(const std::string& raw, llm::ChatMessage& out) {
    rapidjson::Document d;
    if (d.Parse(raw.c_str()).HasParseError() || !d.IsObject()) return false;
    out = llm::ChatMessage();
    rapidjson::Value::ConstMemberIterator it = d.FindMember("role");
    if (it != d.MemberEnd() && it->value.IsString()) {
        out.role.assign(it->value.GetString(), it->value.GetStringLength());
    }
    it = d.FindMember("content");
    if (it != d.MemberEnd() && it->value.IsString()) {
        out.content.assign(it->value.GetString(), it->value.GetStringLength());
    }
    return !out.role.empty();
}

}   // namespace

//------------------------------------------------------------------------------

std::vector<llm::ChatMessage> history_load(cache::Cache& cache, std::int64_t user_id,
                                           const std::string& session_id) {
    std::vector<llm::ChatMessage> out;
    if (user_id <= 0 || session_id.empty()) return out;
    const std::vector<std::string> rows =
        cache.lrange(key_for(user_id, session_id), 0, -1);
    out.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        llm::ChatMessage m;
        if (parse_one(rows[i], m)) out.push_back(m);
    }
    return out;
}

//------------------------------------------------------------------------------

void history_append(cache::Cache& cache, std::int64_t user_id,
                    const std::string& session_id,
                    const std::vector<llm::ChatMessage>& turns) {
    if (user_id <= 0 || session_id.empty() || turns.empty()) return;

    std::vector<std::string> rows;
    rows.reserve(turns.size());
    for (std::size_t i = 0; i < turns.size(); ++i) {
        if (turns[i].role.empty()) continue;
        rows.push_back(serialize_one(turns[i]));
    }
    if (rows.empty()) return;

    const std::string key = key_for(user_id, session_id);
    cache.rpush(key, rows);
    cache.ltrim(key, -static_cast<std::int64_t>(kHistoryMaxMessages), -1);
    cache.expire(key, kHistoryTtl);
}

}}
