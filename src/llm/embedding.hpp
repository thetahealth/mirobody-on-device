#pragma once

#include "config/config.hpp"

#include <string>
#include <vector>

namespace mirobody { namespace embedding {

// Provider-agnostic, config-driven text embedding -- the C++ port of the Python
// reference (mirobody/utils/embedding.py). The provider is chosen by the
// EMBEDDING_PROVIDER config key ("gemini" | "qwen"; default "gemini"):
//
//   - gemini -> Google's gemini-embedding-001 via the AI Studio
//     batchEmbedContents endpoint (key from GEMINI_API_KEY / GOOGLE_API_KEY).
//   - qwen   -> Alibaba DashScope text-embedding-v4 over its OpenAI-compatible
//     /embeddings endpoint (key from DASHSCOPE_API_KEY).
//
// All vectors are 1024-dimensional. Calls are synchronous and blocking (a long
// input list is chunked per provider batch limit, with retry/backoff on
// transient HTTP errors), so run this off the server's service thread.

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
