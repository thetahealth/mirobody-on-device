#include "chat/service.hpp"

#include "chat/agent.hpp"   // agent_registry (provider discovery)
#include "chat/transport/sse.hpp"
#include "chat/transport/ws.hpp"
// #include "chat/transport/mqtt.hpp"   // enable once a broker client is built
#include "database/enums.hpp"   // MessageRole, ShareAccess
#include "platform/clock.hpp"   // now_unix_ms
#include "platform/log.hpp"
#include "server/auth.hpp"
#include "transcode/file.hpp"   // file::list, FileRef, iso8601_utc

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace mirobody { namespace chat {

namespace {

// Read a string member from a JSON object body, or "" if absent / not an object.
// A number (a conversation id sent as a JS number) is returned as decimal text,
// so id binding stays uniform regardless of how the client encoded it.
std::string body_str(const std::string& body, const char* key) {
    rapidjson::Document d;
    if (d.Parse(body.c_str()).HasParseError() || !d.IsObject()) return std::string();
    rapidjson::Value::ConstMemberIterator it = d.FindMember(key);
    if (it == d.MemberEnd()) return std::string();
    if (it->value.IsString()) return std::string(it->value.GetString(), it->value.GetStringLength());
    if (it->value.IsInt64() || it->value.IsInt()) return std::to_string(it->value.GetInt64());
    return std::string();
}

std::string body_session_id(const std::string& body) { return body_str(body, "session_id"); }

#if !defined(MIROBODY_DATABASE_PG_LEGACY)
// database::MessageRole int -> the role string the client renders.
const char* role_str(int role) {
    switch (static_cast<database::MessageRole>(role)) {
        case database::MessageRole::User:      return "user";
        case database::MessageRole::Assistant: return "assistant";
        case database::MessageRole::System:    return "system";
        case database::MessageRole::Tool:      return "tool";
        default:                               return "";
    }
}
#endif

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
    , dispatcher_(chat_, storage, &cache, parser_.get(), &db, memory,
                  cfg.chat.rate_max_per_window, cfg.chat.rate_window_seconds)
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

    // The full message thread of one conversation, for its owner or a care-circle
    // member it was shared with. GET|POST like the other reads.
    router.http("/api/conversation", server::require_auth(jwt_,
        [this](const server::Request& req, server::Response& res) {
            handle_conversation(req, res);
        }), server::GET | server::POST);

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
    // Provider list for every public agent that has a client loaded, GROUPED by
    // agent: each entry is { agent, providers[] }. The default agent carries an
    // EMPTY agent name (the server drops its name from provider_names), so the
    // picker shows just the models and the turn routes through the default agent
    // (see resolve_agent). A client submits the chosen {agent, provider} verbatim.
    const std::vector<std::string> names = agent_registry().provider_names(/*public_only=*/true);

    // Group the flat "agent/provider" (or bare, for the default agent) list by
    // agent. std::map keeps agents ordered, with the default agent's empty name
    // first; providers keep provider_names' sorted order within each group.
    std::map<std::string, std::vector<std::string> > groups;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& full  = names[i];
        const std::size_t  slash = full.find('/');
        const std::string  agent    = (slash == std::string::npos) ? std::string() : full.substr(0, slash);
        const std::string  provider = (slash == std::string::npos) ? full : full.substr(slash + 1);
        groups[agent].push_back(provider);
    }

    rapidjson::Document d;
    d.SetArray();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    for (std::map<std::string, std::vector<std::string> >::const_iterator g = groups.begin();
         g != groups.end(); ++g) {
        rapidjson::Value item(rapidjson::kObjectType);
        item.AddMember("agent",
                       rapidjson::Value(g->first.c_str(),
                                        static_cast<rapidjson::SizeType>(g->first.size()), a), a);
        rapidjson::Value provs(rapidjson::kArrayType);
        for (std::size_t j = 0; j < g->second.size(); ++j) {
            const std::string& p = g->second[j];
            provs.PushBack(rapidjson::Value(p.c_str(), static_cast<rapidjson::SizeType>(p.size()), a), a);
        }
        item.AddMember("providers", provs, a);
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

        // Copy a std::string into the document as a JSON string member.
        auto addstr = [&a](rapidjson::Value& obj, const char* k, const std::string& v) {
            obj.AddMember(rapidjson::StringRef(k),
                          rapidjson::Value(v.c_str(), static_cast<rapidjson::SizeType>(v.size()), a), a);
        };

        try {
#if defined(MIROBODY_DATABASE_PG_LEGACY)
            // Legacy: owner-only, no care-circle sharing.
            database::Result r = db_.execute(
                "SELECT session_id, summary, created_at FROM th_sessions "
                "WHERE user_id=? ORDER BY created_at DESC LIMIT ? OFFSET ?;",
                {uid_text, page_size, offset});
            for (std::size_t i = 0; i < r.rows.size(); ++i) {
                const std::vector<database::Value>& row = r.rows[i];
                const database::Value& sid_v = row[0];
                const std::string sid =
                    sid_v.is_null() ? std::string()
                    : (sid_v.type() == database::Value::Type::Int) ? std::to_string(sid_v.as_int())
                    : sid_v.as_text();
                const std::string summary = row[1].is_null() ? std::string() : row[1].as_text();

                rapidjson::Value item(rapidjson::kObjectType);
                addstr(item, "session_id", sid);
                // created_at is a TIMESTAMPTZ string on this legacy backend.
                const database::Value& ts_v = row[2];
                rapidjson::Value ts_json;
                if (ts_v.is_null())                            ts_json.SetNull();
                else if (ts_v.type() == database::Value::Type::Int) ts_json.SetInt64(ts_v.as_int());
                else { const std::string ts = ts_v.as_text();
                       ts_json.SetString(ts.c_str(), static_cast<rapidjson::SizeType>(ts.size()), a); }
                item.AddMember("timestamp", ts_json, a);
                addstr(item, "summary", summary);
                addstr(item, "query_user_id", uid_text);
                item.AddMember("owned", true, a);
                summaries.PushBack(item, a);
            }
#else
            // Owned conversations plus those shared *to* the caller by a
            // care-circle member, newest activity first. Each row is tagged so
            // the client can badge "shared by <owner>" / "shared with N". The
            // three bound uids are: the owned-flag CASE, the owner filter, and
            // the shared-to-me subquery -- in that placeholder order.
            database::Result r = db_.execute(
                "SELECT c.id, c.summary, COALESCE(c.updated_at, c.created_at), c.user_id, "
                "(CASE WHEN c.user_id=? THEN 1 ELSE 0 END), u.email, "
                "(SELECT count(*) FROM conversation_shares s "
                "   WHERE s.conversation_id=c.id AND s.deleted_at IS NULL) "
                "FROM conversations c LEFT JOIN users u ON u.id=c.user_id "
                "WHERE c.deleted_at IS NULL AND (c.user_id=? OR c.id IN "
                "  (SELECT conversation_id FROM conversation_shares "
                "     WHERE shared_with_user_id=? AND deleted_at IS NULL)) "
                "ORDER BY COALESCE(c.updated_at, c.created_at) DESC LIMIT ? OFFSET ?;",
                {uid_text, uid_text, uid_text, page_size, offset});
            for (std::size_t i = 0; i < r.rows.size(); ++i) {
                const std::vector<database::Value>& row = r.rows[i];
                const std::string sid       = row[0].is_null() ? std::string() : std::to_string(row[0].as_int());
                const std::string summary   = row[1].is_null() ? std::string() : row[1].as_text();
                const std::string owner_id  = row[3].is_null() ? std::string() : std::to_string(row[3].as_int());
                const bool        owned     = !row[4].is_null() && row[4].as_int() != 0;
                const std::string owner_em  = (!row[5].is_null()) ? row[5].as_text() : std::string();
                const std::int64_t shares   = row[6].is_null() ? 0 : row[6].as_int();

                rapidjson::Value item(rapidjson::kObjectType);
                addstr(item, "session_id", sid);
                if (!row[2].is_null()) item.AddMember("timestamp", static_cast<std::int64_t>(row[2].as_int()), a);
                else                   { rapidjson::Value n; n.SetNull(); item.AddMember("timestamp", n, a); }
                addstr(item, "summary", summary);
                addstr(item, "query_user_id", owner_id);   // whose conversation it is
                item.AddMember("owned", owned, a);
                if (owned) {
                    item.AddMember("shared_with_count", static_cast<int>(shares), a);
                } else {
                    addstr(item, "shared_by", owner_em);   // owner's email, for the badge
                }
                summaries.PushBack(item, a);
            }
#endif
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

void ChatService::handle_conversation(const server::Request& req, server::Response& res) {
    const std::int64_t uid = req.user_id;
#if defined(MIROBODY_DATABASE_PG_LEGACY)
    (void)uid;
    res.error(-1, "conversation fetch is not supported on this backend");
    return;
#else
    // id from ?id= (GET) or the JSON body (POST), bound as a decimal string (the
    // same id convention as the rest of the history endpoints).
    std::string id = req.query_str("id", "");
    if (id.empty()) id = body_str(req.body, "id");
    if (uid <= 0 || id.empty()) {
        res.error(-1, "missing conversation id");
        return;
    }
    const std::string u = std::to_string(uid);

    try {
        // Owner + summary + owner email. Missing / soft-deleted => not found.
        database::Result meta = db_.execute(
            "SELECT c.user_id, c.summary, u.email FROM conversations c "
            "LEFT JOIN users u ON u.id=c.user_id "
            "WHERE c.id=? AND c.deleted_at IS NULL;",
            {id});
        if (meta.rows.empty() || meta.rows[0].empty() || meta.rows[0][0].is_null()) {
            res.error(-1, "conversation not found");
            return;
        }
        const std::int64_t owner_id = meta.rows[0][0].as_int();
        const std::string  summary  = meta.rows[0][1].is_null() ? std::string() : meta.rows[0][1].as_text();
        const std::string  owner_em = (meta.rows[0].size() > 2 && !meta.rows[0][2].is_null())
                                          ? meta.rows[0][2].as_text() : std::string();
        const bool         owned    = (owner_id == uid);

        // Access: owner, or an active share to the caller. Anything else reports
        // not-found (don't reveal a conversation the caller may not see).
        std::string access = "owner";
        if (!owned) {
            database::Result sh = db_.execute(
                "SELECT access_level FROM conversation_shares "
                "WHERE conversation_id=? AND shared_with_user_id=? AND deleted_at IS NULL;",
                {id, u});
            if (sh.rows.empty() || sh.rows[0].empty()) {
                res.error(-1, "conversation not found");
                return;
            }
            const int lvl = sh.rows[0][0].is_null() ? static_cast<int>(database::ShareAccess::View)
                                                    : static_cast<int>(sh.rows[0][0].as_int());
            access = (lvl == static_cast<int>(database::ShareAccess::Edit)) ? "edit" : "view";
        }

        // The whole thread: the root question (its id IS the conversation id) plus
        // every message that points at the conversation, oldest first.
        database::Result ms = db_.execute(
            "SELECT role, content, agent, provider, created_at FROM messages "
            "WHERE (id=? OR conversation_id=?) AND deleted_at IS NULL "
            "ORDER BY created_at ASC, id ASC;",
            {id, id});

        rapidjson::Document d;
        d.SetObject();
        rapidjson::Document::AllocatorType& a = d.GetAllocator();
        auto addstr = [&a, &d](const char* k, const std::string& v) {
            d.AddMember(rapidjson::StringRef(k),
                        rapidjson::Value(v.c_str(), static_cast<rapidjson::SizeType>(v.size()), a), a);
        };
        addstr("id", id);
        addstr("summary", summary);
        // `owned` + `shared_by` (email) tell the client what it needs; the owner's
        // users PK is not serialized.
        d.AddMember("owned", owned, a);
        addstr("access", access);
        addstr("shared_by", owned ? std::string() : owner_em);

        rapidjson::Value msgs(rapidjson::kArrayType);
        for (std::size_t i = 0; i < ms.rows.size(); ++i) {
            const std::vector<database::Value>& row = ms.rows[i];
            const int role = row[0].is_null() ? 0 : static_cast<int>(row[0].as_int());
            rapidjson::Value m(rapidjson::kObjectType);
            auto madd = [&a, &m](const char* k, const std::string& v) {
                m.AddMember(rapidjson::StringRef(k),
                            rapidjson::Value(v.c_str(), static_cast<rapidjson::SizeType>(v.size()), a), a);
            };
            madd("role",     std::string(role_str(role)));
            madd("content",  row[1].is_null() ? std::string() : row[1].as_text());
            madd("agent",    row[2].is_null() ? std::string() : row[2].as_text());
            madd("provider", row[3].is_null() ? std::string() : row[3].as_text());
            if (!row[4].is_null()) m.AddMember("created_at", static_cast<std::int64_t>(row[4].as_int()), a);
            msgs.PushBack(m, a);
        }
        d.AddMember("messages", msgs, a);

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        d.Accept(w);
        res.ok(std::string(buf.GetString(), buf.GetSize()));
    } catch (const std::exception& e) {
        platform::log_warn("conversation: query failed: %s", e.what());
        res.error(-1, "conversation query failed");
    }
#endif
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

    // Keys bind as decimal strings: that matches the text columns on
    // sqlite/pg_legacy, and pg casts them to its BIGINT conversation id / user_id.
    const std::string u = std::to_string(user_id);

#if defined(MIROBODY_DATABASE_PG_LEGACY)
    db_.execute("DELETE FROM th_sessions WHERE session_id=? AND user_id=?;",
                {session_id, u});
#else
    // The owner removes the conversation; a member it was shared with only drops
    // their own share (it stays for the owner). Decide by ownership first so a
    // member can never delete someone else's conversation.
    database::Result own = db_.execute(
        "SELECT 1 FROM conversations WHERE id=? AND user_id=? AND deleted_at IS NULL;",
        {session_id, u});
    if (!own.rows.empty()) {
        db_.execute("DELETE FROM conversations WHERE id=? AND user_id=?;", {session_id, u});
    } else {
        db_.execute(
            "UPDATE conversation_shares SET deleted_at=? "
            "WHERE conversation_id=? AND shared_with_user_id=? AND deleted_at IS NULL;",
            {platform::now_unix_ms(), session_id, u});
    }
#endif
    return true;
}

}}
