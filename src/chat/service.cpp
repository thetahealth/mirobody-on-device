#include "chat/service.hpp"

#include "chat/agent.hpp"   // agent_registry (provider discovery)
#include "chat/transport/sse.hpp"
#include "chat/transport/ws.hpp"
// #include "chat/transport/mqtt.hpp"   // enable once a broker client is built
#include "platform/log.hpp"
#include "server/auth.hpp"
#include "transcode/file.hpp"   // file::list, FileRef, iso8601_utc

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace mirobody { namespace chat {

namespace {

// Read "session_id" from a JSON object body, or "" if absent / not an object.
std::string body_session_id(const std::string& body) {
    rapidjson::Document d;
    if (d.Parse(body.c_str()).HasParseError() || !d.IsObject()) return std::string();
    rapidjson::Value::ConstMemberIterator it = d.FindMember("session_id");
    if (it != d.MemberEnd() && it->value.IsString()) {
        return std::string(it->value.GetString(), it->value.GetStringLength());
    }
    return std::string();
}

}   // namespace

//------------------------------------------------------------------------------

ChatService::ChatService(server::Router& router,
                         const Config& cfg,
                         database::Database& db,
                         cache::Cache& cache,
                         storage::Storage* storage,
                         memory::Memory* memory,
                         const jwt::Jwt& jwt)
    : chat_(cfg, db)
    , parser_(file::make_parser(cfg))
    , dispatcher_(chat_, storage, &cache, parser_.get(), &db, memory)
    , db_(db)
    , storage_(storage)
    , jwt_(jwt) {

    // One transport per enabled streaming protocol, each parsing its wire into a
    // Packet and handing it to dispatcher_. They register their own routes (SSE:
    // POST /api/chat, WS: GET /api/chat) when started. Uploads are handled by the
    // dispatcher (which owns `storage`), so the transports just carry bytes.
    transports_.push_back(std::unique_ptr<Transport>(new SseTransport(router, dispatcher_, jwt_)));
    transports_.push_back(std::unique_ptr<Transport>(new WsTransport(router, dispatcher_, jwt_)));
    // transports_.push_back(std::unique_ptr<Transport>(new MqttTransport(dispatcher_, cfg)));

    for (std::size_t i = 0; i < transports_.size(); ++i) {
        transports_[i]->start();
        platform::log_info("chat: started '%s' transport", transports_[i]->name());
    }

    register_rest_routes(router);
}

//------------------------------------------------------------------------------

void ChatService::register_rest_routes(server::Router& router) {
    // Both GET and POST are accepted on the discovery / history reads so the web
    // client's POST-only JSON helper can call them; the response is the standard
    // {code,msg,data} envelope.
    //
    // /api/providers is JWT-guarded: it exposes the configured agent/provider
    // (model) names, so it must only be reachable after login.
    router.http("/api/providers", server::require_auth(jwt_,
        [this](const server::Request& req, server::Response& res) {
            handle_providers(req, res);
        }), server::GET | server::POST);

    router.http("/api/history", server::require_auth(jwt_,
        [this](const server::Request& req, server::Response& res) {
            handle_history(req, res);
        }), server::GET | server::POST);
    router.post("/api/history/delete", server::require_auth(jwt_,
        [this](const server::Request& req, server::Response& res) {
            handle_history_delete(req, res);
        }));

    // The caller's uploaded files, with browser-fetchable signed URLs for the
    // raw bytes and the extracted text. GET|POST like the other reads.
    router.http("/api/files", server::require_auth(jwt_,
        [this](const server::Request& req, server::Response& res) {
            handle_files(req, res);
        }), server::GET | server::POST);
}

//------------------------------------------------------------------------------

void ChatService::handle_providers(const server::Request& req, server::Response& res) {
    (void)req;
    // "agent/provider" pairs for every public agent that has a client loaded.
    const std::vector<std::string> names = agent_registry().provider_names(/*public_only=*/true);

    rapidjson::Document d;
    d.SetArray();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& full  = names[i];                 // "agent/provider"
        const std::size_t  slash = full.find('/');
        const std::string  code  = (slash == std::string::npos) ? full : full.substr(slash + 1);

        rapidjson::Value item(rapidjson::kObjectType);
        item.AddMember("code",
                       rapidjson::Value(code.c_str(), static_cast<rapidjson::SizeType>(code.size()), a), a);
        item.AddMember("name",
                       rapidjson::Value(full.c_str(), static_cast<rapidjson::SizeType>(full.size()), a), a);
        d.PushBack(item, a);
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    d.Accept(w);
    res.ok(std::string(buf.GetString(), buf.GetSize()));
}

//------------------------------------------------------------------------------

void ChatService::handle_history(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;               // decoded raw row id
    const std::string  uid_text = std::to_string(uid);  // bind as decimal string (works on text + bigint user_id)

    rapidjson::Document d;
    d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    rapidjson::Value summaries(rapidjson::kArrayType);

    if (uid > 0) {
        int page      = req.query_int("page", 0);
        int page_size = req.query_int("page_size", 20);
        if (page < 0) page = 0;
        if (page_size <= 0 || page_size > 100) page_size = 20;
        const int offset = page * page_size;

        try {
            // conversations.user_id is bigint on pg, text on sqlite/pg_legacy; a decimal string
            // binds to all of them (pg casts it), so bind the id as its decimal string.
            database::Result r = db_.execute(
                "SELECT " MIROBODY_CONVERSATION_ID_COL ", summary, created_at "
                "FROM " MIROBODY_CONVERSATIONS_TABLE " "
                "WHERE user_id=? ORDER BY created_at DESC LIMIT ? OFFSET ?;",
                {uid_text, page_size, offset});
            for (std::size_t i = 0; i < r.rows.size(); ++i) {
                const std::vector<database::Value>& row = r.rows[i];
                // The conversation id is a BIGINT on the modern backends and a
                // string on pg_legacy; render either as the (opaque, decimal)
                // string the client sends back (as "session_id") to /api/history/delete.
                const database::Value& sid_v = row[0];
                const std::string sid =
                    sid_v.is_null() ? std::string()
                    : (sid_v.type() == database::Value::Type::Int) ? std::to_string(sid_v.as_int())
                    : sid_v.as_text();
                const std::string summary = row[1].is_null() ? std::string() : row[1].as_text();

                rapidjson::Value item(rapidjson::kObjectType);
                item.AddMember("session_id",
                               rapidjson::Value(sid.c_str(), static_cast<rapidjson::SizeType>(sid.size()), a), a);
                // created_at: an epoch-ms integer on the modern backends; a
                // timestamp string on pg_legacy (which keeps TIMESTAMPTZ).
                const database::Value& ts_v = row[2];
                rapidjson::Value ts_json;
                if (ts_v.is_null()) {
                    ts_json.SetNull();
                } else if (ts_v.type() == database::Value::Type::Int) {
                    ts_json.SetInt64(ts_v.as_int());
                } else {
                    const std::string ts = ts_v.as_text();
                    ts_json.SetString(ts.c_str(), static_cast<rapidjson::SizeType>(ts.size()), a);
                }
                item.AddMember("timestamp", ts_json, a);
                item.AddMember("summary",
                               rapidjson::Value(summary.c_str(), static_cast<rapidjson::SizeType>(summary.size()), a), a);
                item.AddMember("query_user_id",
                               rapidjson::Value(uid_text.c_str(), static_cast<rapidjson::SizeType>(uid_text.size()), a), a);
                summaries.PushBack(item, a);
            }
        } catch (const std::exception& e) {
            platform::log_warn("history: query failed: %s", e.what());
            res.error(-1, "history query failed");
            return;
        }
    }

    d.AddMember("summaries", summaries, a);
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    d.Accept(w);
    res.ok(std::string(buf.GetString(), buf.GetSize()));
}

void ChatService::handle_history_delete(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
    const std::string  sid = body_session_id(req.body);
    if (uid <= 0 || sid.empty()) {
        res.error(-1, "missing session id");
        return;
    }

    try {
        delete_history(uid, sid);
    } catch (const std::exception& e) {
        platform::log_warn("history: delete failed: %s", e.what());
        res.error(-2, "history delete failed");
        return;
    }
    res.ok();
}

void ChatService::handle_files(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;

    // Pagination + sort, the same query-param convention as /api/history.
    // sort=name|time (default time), order=asc|desc (default desc): the `files`
    // table is indexed on (user_id, created_at) and (user_id, filename).
    int page = req.query_int("page", 0);
    int size = req.query_int("size", 20);
    if (page < 0) page = 0;
    if (size <= 0 || size > 100) size = 20;
    const std::string sort  = req.query_str("sort", "time");
    const std::string order = req.query_str("order", "desc");
    const std::string sort_col   = (sort == "name") ? "filename" : "created_at";
    const bool        descending = (order != "asc");

    std::vector<file::FileRow> rows;
    std::int64_t total = 0;
    if (uid > 0) {
        try {
            rows  = file::db_list_files(db_, uid, sort_col, descending, size, page * size);
            total = file::db_count_files(db_, uid);
        } catch (const std::exception& e) {
            platform::log_warn("files: query failed: %s", e.what());
            res.error(-1, "files query failed");
            return;
        }
    }

    rapidjson::Document d;
    d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    rapidjson::Value files(rapidjson::kArrayType);

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const file::FileRow& f = rows[i];

        // Fresh signed URLs (each an HMAC), minted only for this page: `url` for
        // the raw bytes, `text_url` for the extracted text when one exists.
        const std::string url =
            (storage_ != nullptr) ? storage_->signed_read_url(f.file_key) : std::string();
        const std::string text_url =
            (storage_ != nullptr && !f.text_key.empty()) ? storage_->signed_read_url(f.text_key)
                                                         : std::string();

        rapidjson::Value item(rapidjson::kObjectType);
        auto add = [&](const char* k, const std::string& v) {
            item.AddMember(rapidjson::StringRef(k),
                           rapidjson::Value(v.c_str(), static_cast<rapidjson::SizeType>(v.size()), a), a);
        };
        add("filename",    f.filename);
        add("mime_type",   f.mime_type);
        add("file_key",    f.file_key);
        add("url",         url);
        add("text_url",    text_url);                // "" when no extracted text
        add("summary",     f.summary);               // "" until post-upload processing fills it
        item.AddMember("uploaded_at", static_cast<int64_t>(f.created_at), a);  // unix milliseconds
        files.PushBack(item, a);
    }

    d.AddMember("files", files, a);
    d.AddMember("total", static_cast<int>(total), a);
    d.AddMember("page",  page, a);
    d.AddMember("size",  size, a);
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    d.Accept(w);
    res.ok(std::string(buf.GetString(), buf.GetSize()));
}

bool ChatService::delete_history(std::int64_t user_id, const std::string& session_id) {
    if (user_id <= 0 || session_id.empty()) return false;

    // Scope the delete to the caller so one user can't remove another's. Both
    // keys bind as decimal strings: that matches the text columns on
    // sqlite/pg_legacy, and pg casts them to its BIGINT conversation id / user_id.
    db_.execute("DELETE FROM " MIROBODY_CONVERSATIONS_TABLE " WHERE " MIROBODY_CONVERSATION_ID_COL "=? AND user_id=?;",
                {session_id, std::to_string(user_id)});
    return true;
}

}}
