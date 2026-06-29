#pragma once

// Chat -- the chat-domain class (Tier 2).
//
// This is the transport-agnostic core of the chat service: it owns the domain
// operations the HTTP/WebSocket interface (chat::ChatService) exposes, but knows
// nothing about routes, SSE framing, or JSON envelopes. It deals in llm::Event
// (streamed through an llm::EventHandler) and plain result structs.
//
//   - response()       run a registered agent for a turn (Tier 3 / Tier 4 below)
//   - live_response()  run a realtime (OpenAI Realtime / Gemini Live) turn
//   - history() / delete_history() / persist_history()   per-user session log
//   - providers()      the "agent/provider" pairs that have a client loaded
//
// Chat *borrows* `cfg` and `db` (it does not own them); both must outlive it.
// The interface layer constructs one Chat and forwards parsed requests to it.

#include "chat/agent.hpp"        // AgentRequest
#include "config/config.hpp"
#include "database/database.hpp"
#include "llm/event.hpp"
#include "llm/mirothinker.hpp"   // ChatMessage, EventHandler

#include <cstdint>
#include <string>
#include <vector>

// The conversation-history table differs by schema: the modern backends
// (res/sql/pg, res/sql/sqlite) call it `conversations` with a BIGINT identity
// `id` key, while the legacy backend (res/sql/pg_legacy) calls it `th_sessions`
// with a string `session_id` key. The chat persistence SQL is otherwise
// identical across backends, so the queries splice the right table and
// key-column names in from these tokens rather than branching the whole statement.
#if defined(MIROBODY_DATABASE_PG_LEGACY)
#define MIROBODY_CONVERSATIONS_TABLE "th_sessions"
#define MIROBODY_CONVERSATION_ID_COL "session_id"
#else
#define MIROBODY_CONVERSATIONS_TABLE "conversations"
#define MIROBODY_CONVERSATION_ID_COL "id"
#endif

namespace mirobody { namespace chat {

//------------------------------------------------------------------------------
// Request structs
//------------------------------------------------------------------------------

// A realtime (live) turn: which provider to bridge, an optional system prompt,
// and the conversation so far.
struct LiveRequest {
    std::string                   provider;
    std::string                   system;
    std::vector<llm::ChatMessage> messages;
};

//------------------------------------------------------------------------------
// Chat
//------------------------------------------------------------------------------

class Chat {
public:
    Chat(const Config& cfg, database::Database& db);

    Chat(const Chat&)            = delete;
    Chat& operator=(const Chat&) = delete;

    // Run the named agent for `req`, streaming its raw llm events through
    // `on_event` (presentation filters run later, at the dispatcher boundary). An
    // unknown agent is reported as an Error event (not thrown). Returning false
    // from `on_event` aborts the in-flight work.
    //
    // Conversation memory: when `req` is identified (user + session + cache)
    // and carries only its current turn, the cached conversation
    // (chat/history.hpp) is prepended to req.messages -- hence the non-const
    // ref -- and the finished question/answer exchange is appended back to
    // the cache after the turn. Clients that send their own multi-turn
    // `messages` bypass it.
    void response(const std::string& agent_name, AgentRequest& req,
                  const llm::EventHandler& on_event);

    // Run one realtime turn: build the provider's client (OpenAI Realtime /
    // Gemini Live) from `cfg_`, ainvoke it, and forward each event through
    // `on_event`. A missing key / unknown provider is reported as an Error
    // event. Does NOT emit a terminal frame -- the caller marks turn end.
    void live_response(const LiveRequest& req, const llm::EventHandler& on_event);

    // Best-effort: record this turn's question so the thread shows in history,
    // and return the conversation (thread) id it landed in -- 0 when nothing was
    // written (anonymous user / empty question / failure / legacy backend).
    //
    // Thread-aware (modern backends): when req.conversation_id names a thread the
    // caller owns, the question is appended to it; otherwise a new thread is
    // started. Either way it stamps req.conversation_id (the thread) and
    // req.question_id (this question's row) so the streamed answer -- persisted
    // by response() once the turn ends -- can link back. Never throws.
    std::int64_t persist_history(AgentRequest& req);

private:
    // Best-effort: persist the finished assistant answer for `req`'s turn as a
    // messages row linked to req.conversation_id / req.question_id (set by
    // persist_history). No-op when those are unset, the answer is empty, or on the
    // legacy backend. Never throws.
    void persist_response(const AgentRequest& req, const std::string& agent_name,
                          const std::string& reply);

    const Config&      cfg_;
    database::Database& db_;
};

//------------------------------------------------------------------------------
// Conversation-summary update (chat-history MCP tool backend)
//------------------------------------------------------------------------------

// Replace the summary of the caller's most recent conversation -- the thread the
// current turn is responding to, since the dispatcher seeds it (persist_history)
// before streaming. This is the backend the summarize_conversation MCP tool calls
// so a model can refine conversations.summary mid-response. Best-effort: returns
// false (never throws) when user_id <= 0, no conversation exists, or the write
// fails. The legacy th_sessions backend has no updated_at column, so only the
// modern conversations table stamps it.
bool update_latest_conversation_summary(database::Database& db,
                                         std::int64_t        user_id,
                                         const std::string&  summary);

}}
