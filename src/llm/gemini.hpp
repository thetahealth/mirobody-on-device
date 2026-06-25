#pragma once

#include "llm/event.hpp"
#include "llm/mirothinker.hpp"   // ChatMessage + EventHandler

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace mirobody { namespace llm {

// Two Gemini surfaces share the same `streamGenerateContent` wire format;
// only the endpoint + auth differ:
//
//   AiStudio: https://generativelanguage.googleapis.com/v1beta/models/{model}:streamGenerateContent
//             auth = ?key={api_key}   (Google AI Studio API key)
//
//   Vertex  : https://{location}-aiplatform.googleapis.com/v1/projects/{project}
//             /locations/{location}/publishers/google/models/{model}:streamGenerateContent
//             auth = Authorization: Bearer {access_token}   (ADC / gcloud)
//
// NOTE: this is *not* the same path as `_ainvoke_interactions` in
// pub/agents/base/clients.py. That code targets Google's experimental
// Interactions API (stateful, MCP-native) via the official genai SDK; that
// API isn't on a stable HTTP surface, so faithfully porting it requires
// either depending on the C++ Google API client or reverse-engineering the
// wire format. Use this client for the standard streamGenerateContent path
// (no native MCP); add an Interactions client later if/when MCP support is
// needed on Gemini.
enum class GeminiMode {
    AiStudio,   // generativelanguage.googleapis.com, API-key auth
    Vertex,     // {region}-aiplatform.googleapis.com, OAuth bearer auth
};

struct GeminiOptions {
    GeminiMode mode = GeminiMode::AiStudio;

    // AiStudio mode: pass an API key here (or set GOOGLE_API_KEY env var).
    std::string api_key;

    // Vertex mode: pre-fetched OAuth access token. The simplest way is:
    //     gcloud auth print-access-token
    // Tokens are short-lived (~60 min); refresh between long runs.
    std::string access_token;

    // Required in Vertex mode, ignored in AiStudio mode.
    std::string gcp_project;
    std::string gcp_location = "us-central1";

    // Optional overrides for the API host. Sensible defaults are picked per
    // mode if left empty.
    std::string ai_studio_base_url;     // default: https://generativelanguage.googleapis.com
    std::string vertex_base_url;        // default: https://{location}-aiplatform.googleapis.com
    std::string api_version = "v1beta"; // AiStudio: v1beta; Vertex: v1

    std::string model = "gemini-2.5-flash";

    // Sampling.
    double temperature = 0.1;

    // Per-request prices in $/M tokens.
    double input_price  = 0.0;
    double output_price = 0.0;

    int         connect_timeout_ms = 10000;
    int         request_timeout_ms = 600000;
    std::size_t min_chunk_size     = 30;

    // Tool use (function calling). When `tools_json` is non-empty AND
    // `tool_executor` is set, `tools_json` is sent as the request's
    // functionDeclarations (just the `[{name,description,parameters}, ...]`
    // array, e.g. mcp::registry().functions_json("gemini") — the client wraps
    // it in the required `tools:[{functionDeclarations:[...]}]` envelope). Each
    // functionCall the model makes is run through `tool_executor(name,
    // args_json, user)` — `user` being the identity passed to ainvoke — its
    // result returned as a functionResponse, and the model re-queried, up to
    // `max_tool_iterations` rounds. Leaving either field unset disables tool use.
    std::string tools_json;
    std::function<std::string(const std::string& name,
                              const std::string& arguments_json,
                              const UserContext& user)> tool_executor;
    int max_tool_iterations = 8;
};

//------------------------------------------------------------------------------
// GeminiClient
//------------------------------------------------------------------------------

class GeminiClient {
public:
    explicit GeminiClient(GeminiOptions opt);

    GeminiClient(const GeminiClient&)            = delete;
    GeminiClient& operator=(const GeminiClient&) = delete;

    bool ainvoke(const std::vector<ChatMessage>& messages,
                 const std::string& system_prompt,
                 const EventHandler& on_event,
                 const UserContext& user = UserContext());

    const GeminiOptions& options() const { return opt_; }

private:
    GeminiOptions opt_;
};

}
}
