#pragma once

// Server-side conversation memory: a per-(user, session) rolling window of
// chat turns in the cache (Redis in production), so a client can send only
// its current question and still get a multi-turn conversation. The provider
// chat APIs themselves (Gemini generateContent et al.) are stateless and need
// the history resent on every call; this is where that history lives when the
// client doesn't carry it.
//
// Storage: one LIST per conversation, mirobody:chat:history:v1:<user>:<sess>,
// one {role, content} JSON object per row, oldest first (each exchange is
// RPUSHed). Text only -- file parts are per-turn payloads and are not
// replayed. Chat::response loads it before the agent runs and appends the
// finished exchange after; see chat.cpp.

#include "cache/cache.hpp"
#include "llm/mirothinker.hpp"   // llm::ChatMessage

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mirobody { namespace chat {

// Cap on rows (one message per row) retained per conversation; the oldest
// fall off. 40 rows = 20 exchanges, comfortably past the 10 user turns
// BaseAgent keeps after trim_to_recent_user_turns.
const std::size_t kHistoryMaxMessages = 40;

// How long a conversation survives without a new exchange.
const std::chrono::hours kHistoryTtl(72);

// The cached conversation, oldest first. Empty when nothing is stored,
// user_id <= 0, or session_id is empty.
std::vector<llm::ChatMessage> history_load(cache::Cache& cache, std::int64_t user_id,
                                           const std::string& session_id);

// Append `turns` (in order) to the conversation, trimming to
// kHistoryMaxMessages and refreshing the TTL. Messages with an empty role are
// skipped. No-op when user_id <= 0, session_id is empty, or nothing remains
// to append.
void history_append(cache::Cache& cache, std::int64_t user_id,
                    const std::string& session_id,
                    const std::vector<llm::ChatMessage>& turns);

}}
