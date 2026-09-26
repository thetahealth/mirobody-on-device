#pragma once

#include "blob.hpp"
#include "llm/event.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace mirobody {
namespace cache    { class Cache; }
namespace storage  { class Storage; }
namespace memory   { class Memory; }
namespace database { class Database; }
namespace llm {

// A file inlined into a turn so the provider can read it directly (e.g. Gemini
// `inlineData`). `data` is the raw, un-encoded bytes; the client base64-encodes
// as the provider requires. Clients that don't support inlining ignore these.
struct FilePart {
    std::string mime_type;   // e.g. "image/png"; empty => application/octet-stream
    // The raw bytes, shared with whoever attached them (see mirobody::Blob) --
    // the caller builds these parts from a const request, so owning a copy here
    // duplicated the whole file for the length of the call.
    Blob data;
};

struct ChatMessage {
    std::string role;       // "user" / "assistant" / "system" / "tool"
    std::string content;
    std::vector<FilePart> files;   // optional inline attachments for this turn
};

struct MiroThinkerOptions {
    std::string api_key;
    std::string base_url        = "https://api.miromind.ai/v1";
    std::string model           = "mirothinker-1-7-deepresearch";
    std::string mcp_server_name = "theta_health";
    std::string mcp_url;        // required: where MiroThinker reaches our MCP

    // Per-request prices in $/M tokens — used to compute CostStatistics.total_cost.
    double input_price  = 0.0;
    double output_price = 0.0;

    int    connect_timeout_ms = 10000;
    int    request_timeout_ms = 600000;   // long-lived streams
    std::size_t min_chunk_size = 30;       // coalesce text deltas before yielding
};

// Returning false from the handler aborts the in-flight HTTP transfer.
using EventHandler = std::function<bool(const Event&)>;

// The caller's identity for a turn, threaded through ainvoke so a client that
// runs tools locally (OpenAIChatClient / GeminiClient) can pass it to its
// tool executor. Kept provider-neutral on purpose: llm/ does not depend on
// mcp/, so the executor receives this rather than an mcp::UserInfo, and the
// agent layer maps one onto the other. A non-positive user_id == unauthenticated.
struct UserContext {
    std::int64_t user_id = 0;     // decoded raw row id
    std::string  session_id;

    // Care-circle "currently for" subject: a member whose health the caller may
    // read this turn (0 == none / self). Forwarded to the tool executor so a
    // health-read tool can default to the subject; never replaces user_id.
    std::int64_t subject_user_id = 0;

    // Borrowed services for a locally-executed tool, forwarded verbatim to the
    // tool executor (which maps them onto mcp::ToolContext). Null when the agent
    // path has none. Pointers only, so llm/ stays decoupled from their defs.
    cache::Cache*       cache   = nullptr;
    storage::Storage*   storage = nullptr;
    memory::Memory*     memory  = nullptr;
    database::Database* db      = nullptr;
};

//------------------------------------------------------------------------------
// MiroThinkerClient
//------------------------------------------------------------------------------

class MiroThinkerClient {
public:
    explicit MiroThinkerClient(MiroThinkerOptions opt);

    MiroThinkerClient(const MiroThinkerClient&)            = delete;
    MiroThinkerClient& operator=(const MiroThinkerClient&) = delete;

    // Streams a chat completion via the OpenAI-compatible Chat Completions
    // endpoint at `${base_url}/chat/completions`, mirroring
    // MiroThinkerClient.ainvoke_aiohttp in the Python reference. The handler
    // fires on the calling thread as bytes arrive — do not block it.
    //
    // Returns true if the request completed (including server-side error
    // events delivered through the handler); false on transport-level
    // failure or if the handler aborted the stream.
    // `user` is accepted for interface parity; MiroThinker runs tools provider-
    // side (over MCP_PUBLIC_URL), so it does not consult it.
    bool ainvoke(const std::vector<ChatMessage>& messages,
                 const std::string& system_prompt,
                 const EventHandler& on_event,
                 const UserContext& user = UserContext());

    const MiroThinkerOptions& options() const { return opt_; }

private:
    MiroThinkerOptions opt_;
};

}
}
