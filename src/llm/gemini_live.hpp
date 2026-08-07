#pragma once

#include "llm/event.hpp"
#include "llm/mirothinker.hpp"   // ChatMessage + EventHandler

#include <cstddef>
#include <string>
#include <vector>

namespace mirobody { namespace llm {

// Client for the Gemini Live API (BidiGenerateContent), Google's bidirectional
// WebSocket surface, used here in text-in / text-out mode. Unlike GeminiClient
// (which POSTs streamGenerateContent over HTTP), this opens a wss:// session,
// sends a setup message then the conversation as one client turn, and streams
// the model's text reply back over the same socket before closing.
//
// Two surfaces share the same wire protocol; only the endpoint + auth differ:
//
//   AiStudio: wss://generativelanguage.googleapis.com
//             /ws/google.ai.generativelanguage.{api_version}.GenerativeService
//             .BidiGenerateContent?key={api_key}
//             setup.model = "models/{model}"
//
//   Vertex  : wss://{vertex host}   (gcp::vertex_host, three shapes by location)
//             /ws/google.cloud.aiplatform.v1beta1.LlmBidiService/BidiGenerateContent
//             auth = Authorization: Bearer {access_token}
//             setup.model = "projects/{project}/locations/{location}
//                            /publishers/google/models/{model}"
//
// Audio / video modalities and the realtimeInput (streaming mic) path are not
// implemented: this client requests responseModalities=["TEXT"] and sends the
// turn as a single clientContent with turnComplete=true, matching the one-shot
// request/response shape of the other llm:: clients.
enum class GeminiLiveMode {
    AiStudio,   // generativelanguage.googleapis.com, API-key auth
    Vertex,     // *.aiplatform.googleapis.com, OAuth bearer auth
};

struct GeminiLiveOptions {
    GeminiLiveMode mode = GeminiLiveMode::AiStudio;

    // AiStudio mode: pass an API key here (or set GOOGLE_API_KEY env var).
    std::string api_key;

    // Vertex mode: pre-fetched OAuth access token (gcloud auth print-access-token).
    std::string access_token;

    // Required in Vertex mode, ignored in AiStudio mode.
    std::string gcp_project;
    std::string gcp_location = "us-central1";

    // Optional host overrides (wss://...). Sensible defaults are picked per mode
    // when left empty.
    std::string ai_studio_base_url;     // default: wss://generativelanguage.googleapis.com
    std::string vertex_base_url;        // default: gcp::vertex_host(gcp_location, "wss")
    std::string api_version = "v1beta"; // AiStudio service-name version

    // Live models differ from the streamGenerateContent ones. AiStudio's common
    // default is "gemini-2.0-flash-live-001"; on Vertex use e.g.
    // "gemini-2.0-flash-live-preview-04-09" (override per mode).
    std::string model = "gemini-2.0-flash-live-001";

    // Sampling.
    double temperature = 0.1;

    // Per-request prices in $/M tokens.
    double input_price  = 0.0;
    double output_price = 0.0;

    int         connect_timeout_ms = 10000;
    int         request_timeout_ms = 600000;   // bounds the whole exchange
    std::size_t min_chunk_size     = 30;        // coalesce reply text deltas

    bool insecure_skip_verify = false;          // wss:// cert/hostname check (debug)
};

//------------------------------------------------------------------------------
// GeminiLiveClient
//------------------------------------------------------------------------------

class GeminiLiveClient {
public:
    explicit GeminiLiveClient(GeminiLiveOptions opt);

    GeminiLiveClient(const GeminiLiveClient&)            = delete;
    GeminiLiveClient& operator=(const GeminiLiveClient&) = delete;

    // Run one turn: connect, send setup + the conversation, stream the reply via
    // on_event, then close. Returns true when the turn completed (including
    // server-side error events delivered through the handler), false on a
    // transport failure, timeout, or if the handler aborted the stream. Same
    // contract as the other llm:: clients' ainvoke().
    bool ainvoke(const std::vector<ChatMessage>& messages,
                 const std::string& system_prompt,
                 const EventHandler& on_event);

    const GeminiLiveOptions& options() const { return opt_; }

private:
    GeminiLiveOptions opt_;
};

}
}
