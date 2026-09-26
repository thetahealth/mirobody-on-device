#include "memory/memory.hpp"

#include "config/config.hpp"
#include "memory/local_memory.hpp"
#include "platform/log.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace mirobody { namespace memory {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}   // namespace

std::unique_ptr<Memory> make_memory(const Config& cfg, database::Database& db) {
    const std::string provider = lower(cfg.memory.provider);

    if (provider == "none" || provider == "off" || provider == "disabled") {
        platform::log_info("memory: disabled (MEMORY_PROVIDER=%s)", cfg.memory.provider.c_str());
        return std::unique_ptr<Memory>();
    }

    if (!provider.empty() && provider != "local") {
        platform::log_warn("memory: unknown MEMORY_PROVIDER '%s'; falling back to local",
                           cfg.memory.provider.c_str());
    }
    platform::log_info("memory: using local backend (cosine over per-user embeddings)");
    return make_local_memory(cfg, db);
}

}}
