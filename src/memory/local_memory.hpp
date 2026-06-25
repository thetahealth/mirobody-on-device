#pragma once

// LocalMemory -- the built-in long-term memory backend. See memory/memory.hpp
// for the role of the Memory interface and how a backend is selected.
//
// Storage: one row per memory in the `memories` table (see res/sql/*/*memory*),
// with the embedding kept as a little-endian float32 BLOB alongside the text.
// recall() loads only the caller's rows (a user_id filter), de-serializes each
// vector, and ranks by cosine similarity in process -- no vector index, no
// extra service, identical SQL on every backend.
//
// The embedder is injected (EmbedFn) rather than called directly so the cosine
// ranking can be unit-tested with a deterministic stub instead of the network;
// make_local_memory() binds the real, config-driven embedding::text_embedding_one.

#include "memory/memory.hpp"

#include <functional>
#include <string>
#include <vector>

namespace mirobody {

struct Config;
namespace database { class Database; }

namespace memory {

// Embeds one text into a vector (1024-dim for the shipped embedders). Returns
// an empty vector for blank input (and writes nothing to *err); on a real
// failure returns empty and writes the reason to *err.
using EmbedFn = std::function<std::vector<float>(const std::string& text, std::string* err)>;

//------------------------------------------------------------------------------

class LocalMemory : public Memory {
public:
    // `db` is borrowed and must outlive this. `default_top_k` is used when
    // recall() is called with top_k <= 0. `embed` must be set.
    LocalMemory(database::Database& db, int default_top_k, EmbedFn embed);

    std::int64_t remember(const RememberInput& in, std::string* err) override;
    std::vector<Record> recall(std::int64_t user_id, const std::string& query,
                               int top_k, std::string* err) override;
    bool forget(std::int64_t user_id, std::int64_t id, std::string* err) override;

private:
    database::Database& db_;
    int                 default_top_k_;
    EmbedFn             embed_;
};

//------------------------------------------------------------------------------

// Build a LocalMemory whose embedder is the config-selected
// embedding::text_embedding_one bound to `cfg`. `cfg` and `db` must outlive it.
std::unique_ptr<Memory> make_local_memory(const Config& cfg, database::Database& db);

}}
