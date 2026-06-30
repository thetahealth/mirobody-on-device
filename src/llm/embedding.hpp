#pragma once

#include "config/config.hpp"

#include <string>
#include <vector>

namespace mirobody { namespace embedding {

// Provider-agnostic, config-driven text embedding -- the C++ port of the Python
// reference (mirobody/utils/embedding.py). The provider is chosen by the
// EMBEDDING_PROVIDER config key ("gemini" | "qwen" | "gemma"; default "gemini"):
//
//   - gemini -> Google's gemini-embedding-001 via the AI Studio
//     batchEmbedContents endpoint (key from GEMINI_API_KEY / GOOGLE_API_KEY).
//   - qwen   -> Alibaba DashScope text-embedding-v4 over its OpenAI-compatible
//     /embeddings endpoint (key from DASHSCOPE_API_KEY).
//   - gemma  -> EmbeddingGemma served locally over an OpenAI-compatible endpoint
//     (Ollama / llama.cpp; EMBEDDING_GEMMA_BASE_URL, default Ollama on :11434).
//     Fully on-device -- no text leaves the host.
//
// Vector dimensionality is per-provider: 1024 for gemini/qwen, EmbeddingGemma's
// native 768 for gemma (or EMBEDDING_GEMMA_DIM). A deployment uses one provider;
// the local-memory recall skips any stored vector whose dimension no longer
// matches, so switching providers degrades gracefully rather than corrupting
// ranking. Calls are synchronous and blocking (a long input list is chunked per
// provider batch limit, with retry/backoff on transient HTTP errors), so run
// this off the server's service thread.

struct EmbeddingResult {
    // One vector per input text, in input order. A blank/whitespace input maps
    // to an empty vector at its index. Empty overall when ok() is false.
    std::vector<std::vector<float>> vectors;
    std::string error;          // empty on success

    bool ok() const { return error.empty(); }
};

// Embed `texts` with the configured provider. Duplicate inputs are embedded
// once and fanned back out, matching the Python helper.
EmbeddingResult text_embedding(const Config& cfg, const std::vector<std::string>& texts);

// Single-text convenience. Returns the vector (empty on blank input); writes the
// failure reason to *err (when non-null) and returns an empty vector on error.
std::vector<float> text_embedding_one(const Config& cfg, const std::string& text,
                                      std::string* err = nullptr);

}}
