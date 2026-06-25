#pragma once

// ZepMemory -- a long-term memory backend backed by Zep (getzep.com), a SOTA
// agent-memory service built on a temporal knowledge graph (Graphiti). Zep
// extracts entities and time-aware facts server-side and ranks them with
// semantic + temporal search, so this adapter forwards text into the user's
// graph and queries it back. Selected by MEMORY_PROVIDER=zep with ZEP_API_KEY
// (and optionally ZEP_BASE_URL for a self-hosted server). See memory/memory.hpp
// for the interface and memory/zep_memory.cpp for the REST contract.

#include "memory/memory.hpp"

namespace mirobody {

struct Config;

namespace memory {

// Build a ZepMemory from cfg.memory (api_key required; base_url defaults to the
// hosted Zep cloud). Returns null when no api_key is configured.
std::unique_ptr<Memory> make_zep_memory(const Config& cfg);

}}
