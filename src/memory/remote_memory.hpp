#pragma once

// RemoteMemory -- a long-term memory backend that delegates to an external
// EverOS-compatible HTTP memory service, so a deployment can run EverOS
// (https://github.com/EverMind-AI/EverOS) or any API-compatible store without
// touching the agent/tool layer. Selected by
// MEMORY_PROVIDER=everos|remote with EVEROS_BASE_URL / EVEROS_API_KEY. See
// memory/memory.hpp for the interface and memory/remote_memory.cpp for the REST
// contract this client speaks.

#include "memory/memory.hpp"

namespace mirobody {

struct Config;

namespace memory {

// Build a RemoteMemory from cfg.memory (base_url + api_key, timeouts from the
// global HTTP settings). Returns null when EVEROS_BASE_URL is empty -- a
// misconfigured remote provider is "memory unavailable", not a hard error.
std::unique_ptr<Memory> make_remote_memory(const Config& cfg);

}}
