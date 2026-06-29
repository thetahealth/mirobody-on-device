#pragma once

#include "llm/event.hpp"
#include "llm/mirothinker.hpp"   // ChatMessage + EventHandler

#include <cstddef>
#include <string>
#include <vector>

namespace mirobody { namespace llm {

// Client for the OpenAI Realtime API ("GPT realtime"), OpenAI's bidirectional
// WebSocket surface, used here in text-in / text-out mode. The shape mirrors
// GeminiLiveClient: it opens a wss:// session, sends a session.update then the
// conversation as conversation.item.create events and a response.create, and
// streams the model's text reply back over the same socket before closing.
//
// Two surfaces share the same wire protocol; only the endpoint + auth differ:
//
//   OpenAI: wss://api.openai.com/v1/realtime?model={model}
//           auth = Authorization: Bearer {api_key}
//                  OpenAI-Beta: realtime=v1   (when beta_header)
//
//   Azure : wss://{azure_endpoint}/openai/realtime
//             ?api-version={api_version}&deployment={deployment}
//           auth = api-key: {api_key}
//
// Audio / video modalities and the input_audio_buffer (streaming mic) path are
// not implemented: this client requests modalities=["text"] and drives a single
// response, matching the one-shot request/response shape of the other llm::
// clients. response.text.delta / response.output_text.delta deltas are coalesced
// into Reply events; usage is read from response.done and emitted as
// CostStatistics.
enum class OpenAIRealtimeMode {
    OpenAI,   // api.openai.com, bearer auth
    Azure,    // {resource}.openai.azure.com, api-key auth
};

struct OpenAIRealtimeOptions {
    OpenAIRealtimeMode mode = OpenAIRealtimeMode::OpenAI;

    // Both modes: the API key (OPENAI_API_KEY / AZURE_OPENAI_API_KEY).
    std::string api_key;

    // Optional host overrides (wss://...). Sensible defaults are picked per mode
    // when left empty.
    std::string openai_base_url;   // default: wss://api.openai.com/v1/realtime

    // Azure mode: the resource host (wss://{resource}.openai.azure.com) and the
    // deployment name; api_version selects the realtime preview surface.
    std::string azure_endpoint;
    std::string azure_deployment;
    std::string api_version = "2024-10-01-preview";

    // Realtime models. OpenAI's GA model is "gpt-realtime"; the long-lived
    // preview is "gpt-4o-realtime-preview". OpenAI mode sends this in the
    // connection URL (?model=...). On Azure the deployment selects the model,
    // so this is not sent on the wire there -- it's used only as the
    // CostStatistics.model label.
    std::string model = "gpt-realtime-mini";

    // Send the "OpenAI-Beta: realtime=v1" header. Required by the preview
    // surface; harmless on GA. Disable for endpoints that reject it.
    bool beta_header = true;

    // Sampling. Sent only when > 0 (the GA surface rejects it on some models).
    double temperature = 0.0;

    // Per-request prices in $/M tokens.
    double input_price  = 0.0;
    double output_price = 0.0;

    int         connect_timeout_ms = 10000;
    int         request_timeout_ms = 600000;   // bounds the whole exchange
    std::size_t min_chunk_size     = 30;        // coalesce reply text deltas

    bool insecure_skip_verify = false;          // wss:// cert/hostname check (debug)
};

//------------------------------------------------------------------------------
// OpenAIRealtimeClient
//------------------------------------------------------------------------------

class OpenAIRealtimeClient {
public:
    explicit OpenAIRealtimeClient(OpenAIRealtimeOptions opt);

    OpenAIRealtimeClient(const OpenAIRealtimeClient&)            = delete;
    OpenAIRealtimeClient& operator=(const OpenAIRealtimeClient&) = delete;

    // Run one turn: connect, send session.update + the conversation, drive one
    // response, stream the reply via on_event, then close. Returns true when the
    // turn completed (including server-side error events delivered through the
    // handler), false on a transport failure, timeout, or if the handler aborted
    // the stream. Same contract as the other llm:: clients' ainvoke().
    bool ainvoke(const std::vector<ChatMessage>& messages,
                 const std::string& system_prompt,
                 const EventHandler& on_event);

    const OpenAIRealtimeOptions& options() const { return opt_; }

private:
    OpenAIRealtimeOptions opt_;
};

}
}
