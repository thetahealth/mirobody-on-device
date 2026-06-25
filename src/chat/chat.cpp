#include "chat/chat.hpp"

#include "chat/history.hpp"
#include "database/enums.hpp"   // MessageRole
#include "llm/gemini_live.hpp"
#include "llm/openai_realtime.hpp"
#include "platform/clock.hpp"   // now_unix_ms
#include "platform/log.hpp"

#include <openssl/rand.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstddef>
#include <memory>
#include <random>

namespace mirobody { namespace chat {

namespace {

#if defined(MIROBODY_DATABASE_PG_LEGACY)
// 128 bits of randomness as lowercase hex, used as a th_sessions.session_id for
// a persisted history row on the legacy backend (whose session_id is a string
// key supplied by the writer). The modern backends auto-assign a BIGINT
// session_id instead, so this is compiled only for the legacy build.
std::string new_history_id() {
    unsigned char b[16];
    if (RAND_bytes(b, sizeof(b)) != 1) {
        std::random_device rd;
        for (std::size_t i = 0; i < sizeof(b); ++i) b[i] = static_cast<unsigned char>(rd());
    }
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(sizeof(b) * 2);
    for (std::size_t i = 0; i < sizeof(b); ++i) {
        s.push_back(kHex[b[i] >> 4]);
        s.push_back(kHex[b[i] & 0x0F]);
    }
    return s;
}
#endif

// Serialize a turn's attachments to the JSON array stored in conversations.files:
// [{"name": <filename>, "key": <file_key>}, ...]. Returns "" for no files, so
// the column stays NULL-equivalent (the reader treats empty/null as "none").
std::string files_json(const std::vector<AgentFile>& files) {
    if (files.empty()) return std::string();
    rapidjson::Document d;
    d.SetArray();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    for (std::size_t i = 0; i < files.size(); ++i) {
        const AgentFile& f = files[i];
        rapidjson::Value o(rapidjson::kObjectType);
        o.AddMember("name",
                    rapidjson::Value(f.filename.c_str(),
                                     static_cast<rapidjson::SizeType>(f.filename.size()), a), a);
        o.AddMember("key",
                    rapidjson::Value(f.file_key.c_str(),
                                     static_cast<rapidjson::SizeType>(f.file_key.size()), a), a);
        d.PushBack(o, a);
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    d.Accept(w);
    return std::string(buf.GetString(), buf.GetSize());
}

// Emit an Error event to `sink` (used for in-band failure reporting).
void emit_error(const llm::EventHandler& sink, const std::string& msg) {
    llm::Event e;
    e.type    = llm::EventType::Error;
    e.content = msg;
    sink(e);
}

}   // namespace

//------------------------------------------------------------------------------

Chat::Chat(const Config& cfg, database::Database& db)
    : cfg_(cfg), db_(db) {}

//------------------------------------------------------------------------------
// Agent turn
//------------------------------------------------------------------------------

void Chat::response(const std::string& agent_name, AgentRequest& req,
                    const llm::EventHandler& on_event) {
    std::unique_ptr<Agent> agent = agent_registry().create(agent_name, req);
    if (!agent) {
        emit_error(on_event, "no such agent: " + agent_name);
        return;
    }

    // Server-side conversation memory (chat/history.hpp): engaged when the
    // request is identified and the client sent only its current turn -- a
    // client that manages its own multi-turn history (messages > 1) carries
    // its context itself and bypasses it.
    const bool memory = req.cache != nullptr && req.user_id > 0 &&
                        !req.session_id.empty() && req.messages.size() <= 1;
    std::string user_text;
    if (memory) {
        // The current turn's text, captured before the prepend below: the
        // question field, or the lone client-sent user message.
        user_text = !req.question.empty() ? req.question
                  : (!req.messages.empty() && req.messages.back().role == "user"
                         ? req.messages.back().content : std::string());

        std::vector<llm::ChatMessage> turns =
            history_load(*req.cache, req.user_id, req.session_id);
        if (!turns.empty()) {
            turns.insert(turns.end(), req.messages.begin(), req.messages.end());
            req.messages.swap(turns);
        }
    }

    // Accumulate the streamed answer so the finished exchange can be appended
    // to the conversation memory once the turn ends.
    std::string reply;
    llm::EventHandler recorder;
    if (memory) {
        recorder = [&reply, &on_event](const llm::Event& e) -> bool {
            if (e.type == llm::EventType::Reply) reply += e.content;
            return on_event(e);
        };
    }

    // Stream the agent's events straight through. Presentation transforms (e.g.
    // collapsing a render_chart tool call into a ChartEvent) are applied by the
    // chat event-filter pipeline at the dispatcher boundary, not here -- this tier
    // deals only in the raw llm event stream.
    agent->generate_response(req, memory ? recorder : on_event);

    // A turn that produced no answer (error, abort) is not recorded -- the
    // question will be resent, and a context entry with no reply teaches the
    // model nothing.
    if (memory && !user_text.empty() && !reply.empty()) {
        std::vector<llm::ChatMessage> exchange;
        exchange.push_back(llm::ChatMessage{"user",      user_text, {}});
        exchange.push_back(llm::ChatMessage{"assistant", reply,     {}});
        history_append(*req.cache, req.user_id, req.session_id, exchange);
    }
}

//------------------------------------------------------------------------------
// Realtime turn
//------------------------------------------------------------------------------

void Chat::live_response(const LiveRequest& req, const llm::EventHandler& on_event) {
    if (req.provider == "openai") {
        if (cfg_.openai.api_key.empty()) {
            emit_error(on_event, "OPENAI_API_KEY not configured");
            return;
        }
        llm::OpenAIRealtimeOptions opt;
        opt.mode               = llm::OpenAIRealtimeMode::OpenAI;
        opt.api_key            = cfg_.openai.api_key;
        if (!cfg_.openai.realtime_url.empty()) opt.openai_base_url = cfg_.openai.realtime_url;
        opt.connect_timeout_ms = cfg_.connect_timeout_ms;
        opt.request_timeout_ms = cfg_.request_timeout_ms;
        llm::OpenAIRealtimeClient client{std::move(opt)};
        client.ainvoke(req.messages, req.system, on_event);
    } else if (req.provider == "gemini") {
        if (cfg_.gemini.api_key.empty()) {
            emit_error(on_event, "GOOGLE_API_KEY not configured");
            return;
        }
        llm::GeminiLiveOptions opt;
        opt.mode               = llm::GeminiLiveMode::AiStudio;
        opt.api_key            = cfg_.gemini.api_key;
        opt.connect_timeout_ms = cfg_.connect_timeout_ms;
        opt.request_timeout_ms = cfg_.request_timeout_ms;
        llm::GeminiLiveClient client{std::move(opt)};
        client.ainvoke(req.messages, req.system, on_event);
    } else {
        emit_error(on_event, "unknown provider: " +
                             (req.provider.empty() ? std::string("(none)") : req.provider));
    }
}

//------------------------------------------------------------------------------
// History
//------------------------------------------------------------------------------

void Chat::persist_history(const AgentRequest& req) {
    if (req.user_id <= 0 || req.question.empty()) return;

    // Summary is the question, capped so a long prompt doesn't bloat the row.
    std::string summary = req.question;
    if (summary.size() > 200) summary.resize(200);

    // Per-question attachments as a JSON array of {name, key}; empty => none.
    const std::string files = files_json(req.files);
    // user_id binds as a decimal string everywhere: text on sqlite/pg_legacy,
    // and bigint on pg/mysql (which cast it).
    const std::string uid = std::to_string(req.user_id);

    try {
#if defined(MIROBODY_DATABASE_PG_LEGACY)
        // Legacy th_sessions: one row keyed on a supplied string session_id,
        // carrying the question detail inline (no separate messages table here).
        db_.execute(
            "INSERT INTO th_sessions (session_id, user_id, summary, language, timezone, files) "
            "VALUES (?, ?, ?, ?, ?, ?);",
            {new_history_id(), uid, summary, req.language, req.timezone, files});
#else
        // Modern: write the opening question as a messages row (conversation_id
        // left NULL -- it is the conversation root), then seed a thin conversations
        // row whose id IS that question's id, so one id names the thread across
        // both tables. The two writes share a transaction so a conversation never
        // exists without its opening question. Agent responses are persisted by
        // the streaming path, not here; each call starts a fresh conversation
        // (continuing an existing thread is future work).
        database::Transaction tx  = db_.begin();
        const int             role = static_cast<int>(database::MessageRole::User);
        const std::int64_t    now  = platform::now_unix_ms();   // created_at/updated_at (unix ms)
  #if defined(MIROBODY_DATABASE_PG)
        // PG has no last_insert_id; read the assigned id back with RETURNING.
        database::Result qr = tx.execute(
            "INSERT INTO messages (user_id, role, content, language, timezone, files, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?) RETURNING id;",
            {uid, role, req.question, req.language, req.timezone, files, now});
        const std::int64_t qid =
            (qr.rows.empty() || qr.rows[0].empty()) ? 0 : qr.rows[0][0].as_int();
  #else
        database::Result qr = tx.execute(
            "INSERT INTO messages (user_id, role, content, language, timezone, files, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?);",
            {uid, role, req.question, req.language, req.timezone, files, now});
        const std::int64_t qid = qr.last_insert_id;
  #endif
        tx.execute(
            // updated_at left NULL until the thread is later touched.
            "INSERT INTO conversations (id, user_id, summary, created_at) "
            "VALUES (?, ?, ?, ?);",
            {qid, uid, summary, now});
        tx.commit();
#endif
    } catch (const std::exception& e) {
        platform::log_warn("history: persist failed: %s", e.what());
    }
}

//------------------------------------------------------------------------------

bool update_latest_conversation_summary(database::Database& db,
                                         std::int64_t        user_id,
                                         const std::string&  summary) {
    if (user_id <= 0) return false;

    // user_id binds as a decimal string everywhere (text on sqlite/pg_legacy,
    // bigint on pg/mysql, which cast it) -- the same convention as persist_history.
    const std::string uid = std::to_string(user_id);

    try {
        // The thread's id is a BIGINT (modern) or a string session_id (legacy);
        // pick the newest and update it by key. A two-step select-then-update is
        // used rather than UPDATE ... ORDER BY ... LIMIT, which PostgreSQL does
        // not support.
        database::Result sel = db.execute(
            "SELECT " MIROBODY_CONVERSATION_ID_COL " "
            "FROM " MIROBODY_CONVERSATIONS_TABLE " "
            "WHERE user_id=? ORDER BY created_at DESC LIMIT 1;",
            {uid});
        if (sel.rows.empty() || sel.rows[0].empty() || sel.rows[0][0].is_null()) {
            return false;   // no conversation to attach the summary to (yet)
        }
        // Render the id as its opaque decimal/string form, the same way the
        // history listing does, so it binds across the int and string key columns.
        const database::Value& idv = sel.rows[0][0];
        const std::string cid =
            (idv.type() == database::Value::Type::Int) ? std::to_string(idv.as_int())
                                                       : idv.as_text();

#if defined(MIROBODY_DATABASE_PG_LEGACY)
        // Legacy th_sessions has no updated_at column.
        db.execute(
            "UPDATE " MIROBODY_CONVERSATIONS_TABLE " SET summary=? "
            "WHERE " MIROBODY_CONVERSATION_ID_COL "=? AND user_id=?;",
            {summary, cid, uid});
#else
        db.execute(
            "UPDATE " MIROBODY_CONVERSATIONS_TABLE " SET summary=?, updated_at=? "
            "WHERE " MIROBODY_CONVERSATION_ID_COL "=? AND user_id=?;",
            {summary, platform::now_unix_ms(), cid, uid});
#endif
        return true;
    } catch (const std::exception& e) {
        platform::log_warn("history: summary update failed: %s", e.what());
        return false;
    }
}

}}
