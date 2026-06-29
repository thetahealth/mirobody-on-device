#pragma once

#include "llm/event.hpp"
#include "llm/mirothinker.hpp"   // ChatMessage + EventHandler

#include <cstddef>
#include <string>
#include <vector>

namespace mirobody { namespace llm {

// OpenAI Responses API streaming client.
//
// Endpoint: POST {base_url}/responses with `stream: true`. The response is
// named-SSE (each event has an `event: response.<name>` line plus a JSON
// `data:` line) rather than the unnamed Chat Completions style.
//
// Mirrors `OpenAIResponsesClient.ainvoke` in pub/agents/base/clients.py, but
// strips out the function-call execution loop — a tool_call surfaces as
// QueryTitle / QueryArguments events and the stream then completes without
// running anything. This is a debugging client.
//
// `previous_response_id` is left unset by default; pass one explicitly to
// continue a stored response chain.

struct OpenAIResponsesOptions {
    std::string api_key;
    std::string base_url = "https://api.openai.com/v1";
    std::string model    = "gpt-5-nano";

    // Optional: continue an earlier stored response.
    std::string previous_response_id;

    // Whether the server should retain the response so it can be referenced
    // via previous_response_id on a follow-up. Required for the chaining flow.
    bool store = true;

    // Per-request prices in $/M tokens.
    double input_price  = 0.0;
    double output_price = 0.0;

    int         connect_timeout_ms = 10000;
    int         request_timeout_ms = 600000;
    std::size_t min_chunk_size     = 30;
};

//------------------------------------------------------------------------------
// OpenAIResponsesClient
//------------------------------------------------------------------------------

class OpenAIResponsesClient {
public:
    explicit OpenAIResponsesClient(OpenAIResponsesOptions opt);

    OpenAIResponsesClient(const OpenAIResponsesClient&)            = delete;
    OpenAIResponsesClient& operator=(const OpenAIResponsesClient&) = delete;

    bool ainvoke(const std::vector<ChatMessage>& messages,
                 const std::string& system_prompt,
                 const EventHandler& on_event,
                 const UserContext& user = UserContext());

    // After a successful stream completes, this holds the most recent
    // response id so callers can chain via previous_response_id.
    const std::string& last_response_id() const { return last_response_id_; }

    const OpenAIResponsesOptions& options() const { return opt_; }

private:
    OpenAIResponsesOptions opt_;
    std::string last_response_id_;
};

}
}
