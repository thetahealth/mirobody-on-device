#pragma once

// Mem0Memory -- a long-term memory backend backed by Mem0 (mem0.ai), one of the
// SOTA agent-memory services. Mem0 does its own LLM-driven fact extraction and
// multi-signal retrieval server-side, so this adapter just forwards the user's
// text and recall queries. Selected by MEMORY_PROVIDER=mem0 with MEM0_API_KEY
// (and optionally MEM0_BASE_URL for a self-hosted server). See memory/memory.hpp
// for the interface and memory/mem0_memory.cpp for the REST contract.

#include "memory/memory.hpp"

namespace mirobody {

struct Config;

namespace memory {

// Build a Mem0Memory from cfg.memory (api_key required; base_url defaults to the
// hosted Mem0 platform). Returns null when no api_key is configured -- a
// misconfigured provider is "memory unavailable", not a hard error.
std::unique_ptr<Memory> make_mem0_memory(const Config& cfg);

}}
