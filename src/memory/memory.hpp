#pragma once

// Long-term memory: store durable facts about a user and recall the most
// relevant ones for a turn. This is the mirobody analog of EverOS's memory
// subsystem (https://github.com/EverMind-AI/EverOS) -- a portable, self-evolving store
// of what the assistant has learned -- pared down to the slice that matters
// here: per-user long-term recall, surfaced to agents as the `remember` /
// `recall_memory` MCP tools (see res/mcp_tools/).
//
// `Memory` is an abstract interface so the backend is swappable from config
// (cfg.memory.provider), exactly as the embedder is (EMBEDDING_PROVIDER):
//
//   - LocalMemory  (memory/local_memory.*) -- the built-in, no-extra-services
//     backend. Facts and their 1024-dim embeddings live in the app database;
//     recall ranks the caller's OWN rows by in-process cosine similarity. A
//     query loads only that user's vectors (a per-user WHERE filter), so the
//     working set is small (~a few MB for thousands of memories) and no whole-
//     table scan or vector index is needed. Works on every SQL backend,
//     including the on-device SQLite build.
//
//   - RemoteMemory (memory/remote_memory.*) -- delegates to an external
//     EverOS-compatible HTTP memory service, so a deployment can run EverOS (or
//     any API-compatible store) without changing the agent/tool layer.
//
// The pgvector / sqlite-vec upgrade path (push ANN search into SQL once one
// user accumulates tens of thousands of memories, or for cross-user search)
// slots in behind this same interface as another backend; LocalMemory's
// per-user in-process cosine is the simple-but-effective default until then.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mirobody {

struct Config;
namespace database { class Database; }

namespace memory {

//------------------------------------------------------------------------------

// One stored memory. `score` is the cosine similarity to the recall query,
// filled by recall() and left 0 by every other path.
struct Record {
    std::int64_t id         = 0;
    std::string  text;
    std::string  kind;          // free-form tag, e.g. "fact", "preference", "episode"
    std::string  session_id;    // session the memory was captured in (may be empty)
    std::int64_t created_at = 0;   // unix seconds
    double       score      = 0.0;
};

//------------------------------------------------------------------------------

// A new memory to store.
struct RememberInput {
    std::int64_t user_id = 0;
    std::string  text;
    std::string  kind = "fact";
    std::string  session_id;
};

//------------------------------------------------------------------------------

// Pluggable long-term memory store. One instance is owned by the server and
// borrowed (as a pointer) by the MCP tool layer; every method takes the
// caller's user_id, so a single store serves all users. Methods report failure
// by return value + *err rather than throwing, so a tool handler can surface a
// clean message. Implementations are safe to use from one thread at a time:
// each request thread holds its own service handles (see mcp::ToolContext) and
// the backends keep no mutable cross-call state beyond the borrowed Database,
// which is itself single-threaded.
class Memory {
public:
    virtual ~Memory() = default;

    // Store `in` as a durable memory. Returns the new record's id (> 0) on
    // success. Blank text is a no-op success (returns 0, no *err written). On
    // failure returns 0 and writes the reason to *err (when non-null).
    virtual std::int64_t remember(const RememberInput& in, std::string* err) = 0;

    // The `top_k` memories of `user_id` most relevant to `query`, best match
    // first. `top_k <= 0` falls back to the configured default. Returns empty
    // when the user has no memories or `query` is blank; on failure returns
    // empty and writes *err.
    virtual std::vector<Record> recall(std::int64_t user_id, const std::string& query,
                                       int top_k, std::string* err) = 0;

    // Delete memory `id` owned by `user_id`. Returns true when a row was
    // removed, false when none matched. On a backend error returns false and
    // writes *err.
    virtual bool forget(std::int64_t user_id, std::int64_t id, std::string* err) = 0;
};

//------------------------------------------------------------------------------

// Build the memory backend named by cfg.memory.provider, borrowing `db` for the
// local backend (the remote backend ignores it). Returns null when memory is
// disabled (provider "none") or a remote provider is misconfigured (e.g.
// "everos" with no EVEROS_BASE_URL) -- callers treat null as "memory
// unavailable" and the tools report so. `cfg` and `db` must outlive the result.
std::unique_ptr<Memory> make_memory(const Config& cfg, database::Database& db);

}}
