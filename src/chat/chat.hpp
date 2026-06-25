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

    // Best-effort: record a session row for an agent turn so it shows in
    // history. No-op for anonymous users / empty questions; never throws.
    void persist_history(const AgentRequest& req);

private:
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
