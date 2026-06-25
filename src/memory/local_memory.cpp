#include "memory/local_memory.hpp"

#include "config/config.hpp"
#include "database/database.hpp"
#include "database/enums.hpp"   // MemoryKind
#include "llm/embedding.hpp"
#include "platform/clock.hpp"   // now_unix_ms
#include "platform/log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <utility>

namespace mirobody { namespace memory {

namespace {

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// memories.kind is stored as database::MemoryKind; the module carries the tool's
// string ("fact" / "preference" / "episode"). Map between the two, defaulting an
// empty or unrecognized tag to Fact (the tool's documented default).
std::int16_t kind_to_int(const std::string& s) {
    if (s == "preference") return static_cast<std::int16_t>(database::MemoryKind::Preference);
    if (s == "episode")    return static_cast<std::int16_t>(database::MemoryKind::Episode);
    return static_cast<std::int16_t>(database::MemoryKind::Fact);
}
const char* kind_to_string(std::int64_t v) {
    switch (static_cast<database::MemoryKind>(v)) {
        case database::MemoryKind::Preference: return "preference";
        case database::MemoryKind::Episode:    return "episode";
        default:                               return "fact";
    }
}

// A vector serialized as raw little-endian float32 -- the BLOB stored in the
// `embedding` column. Endianness is not normalized: the bytes are only ever
// read back by this same process/build, never shipped between machines, so the
// host's native float layout is fine and avoids per-element byte twiddling.
std::string vec_to_blob(const std::vector<float>& v) {
    std::string blob;
    blob.resize(v.size() * sizeof(float));
    if (!v.empty()) std::memcpy(&blob[0], v.data(), blob.size());
    return blob;
}

std::vector<float> blob_to_vec(const std::string& blob) {
    std::vector<float> v;
    const std::size_t n = blob.size() / sizeof(float);
    v.resize(n);
    if (n) std::memcpy(v.data(), blob.data(), n * sizeof(float));
    return v;
}

// Cosine similarity of two equal-length vectors. Returns 0 when either has zero
// magnitude (so a degenerate/empty vector never ranks above a real match).
double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na  += static_cast<double>(a[i]) * a[i];
        nb  += static_cast<double>(b[i]) * b[i];
    }
    if (na == 0.0 || nb == 0.0) return 0.0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

}   // namespace

//------------------------------------------------------------------------------
// LocalMemory
//------------------------------------------------------------------------------

LocalMemory::LocalMemory(database::Database& db, int default_top_k, EmbedFn embed)
    : db_(db)
    , default_top_k_(default_top_k > 0 ? default_top_k : 5)
    , embed_(std::move(embed)) {}

std::int64_t LocalMemory::remember(const RememberInput& in, std::string* err) {
    if (in.user_id <= 0) {
        if (err) *err = "memory: a logged-in user is required";
        return 0;
    }
    const std::string text = trim(in.text);
    if (text.empty()) return 0;   // nothing to store; not an error

    std::string e;
    const std::vector<float> vec = embed_(text, &e);
    if (!e.empty()) {
        if (err) *err = "memory: embedding failed: " + e;
        return 0;
    }
    if (vec.empty()) {
        if (err) *err = "memory: embedding unavailable (no embedder configured)";
        return 0;
    }

    const std::int16_t kind = kind_to_int(in.kind);   // string tag -> MemoryKind int
    try {
        database::Result r = db_.execute(
            // updated_at is left NULL on insert (set only when a row is later
            // changed, e.g. forget); created_at carries the insert time.
            "INSERT INTO memories (user_id, kind, content, conversation_id, embedding, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            // conversation_id is a BIGINT: bind the handle as a decimal string
            // (the backend casts it, as with user_id), or NULL when none was given.
            { in.user_id, static_cast<int>(kind), text,
              in.session_id.empty() ? database::Value(nullptr) : database::Value(in.session_id),
              database::Value::blob(vec_to_blob(vec)), platform::now_unix_ms() });
        return r.last_insert_id;
    } catch (const std::exception& ex) {
        if (err) *err = std::string("memory: store failed: ") + ex.what();
        return 0;
    }
}

std::vector<Record> LocalMemory::recall(std::int64_t user_id, const std::string& query,
                                        int top_k, std::string* err) {
    std::vector<Record> out;
    if (user_id <= 0) return out;
    const std::string q = trim(query);
    if (q.empty()) return out;
    if (top_k <= 0) top_k = default_top_k_;

    std::string e;
    const std::vector<float> qv = embed_(q, &e);
    if (!e.empty()) {
        if (err) *err = "memory: embedding failed: " + e;
        return out;
    }
    if (qv.empty()) {
        if (err) *err = "memory: embedding unavailable (no embedder configured)";
        return out;
    }

    database::Result r;
    try {
        // Only the caller's rows are loaded -- the per-user filter is what keeps
        // the in-process cosine cheap (no whole-table scan, no vector index).
        r = db_.execute(
            "SELECT id, kind, content, conversation_id, embedding, created_at "
            "FROM memories WHERE user_id = ? AND deleted_at IS NULL",
            { user_id });
    } catch (const std::exception& ex) {
        if (err) *err = std::string("memory: query failed: ") + ex.what();
        return out;
    }

    std::vector<Record> scored;
    scored.reserve(r.rows.size());
    for (std::size_t i = 0; i < r.rows.size(); ++i) {
        const std::vector<database::Value>& row = r.rows[i];
        if (row.size() < 6) continue;
        const std::vector<float> v = blob_to_vec(row[4].as_blob());
        // Skip rows whose dimensionality no longer matches the active embedder
        // (e.g. the embedding provider was changed after they were written) --
        // they simply drop out of recall rather than corrupt the ranking.
        if (v.size() != qv.size()) continue;

        Record rec;
        rec.id         = row[0].as_int();
        rec.kind       = kind_to_string(row[1].as_int());
        rec.text       = row[2].as_text();
        // conversation_id comes back as an integer (BIGINT) on pg/mysql and as an
        // int/null on sqlite; render it into the string session_id field, "" if NULL.
        const database::Value& cv = row[3];
        rec.session_id = cv.is_null() ? std::string()
                       : (cv.type() == database::Value::Type::Int) ? std::to_string(cv.as_int())
                       : cv.as_text();
        rec.created_at = row[5].as_int();
        rec.score      = cosine(qv, v);
        scored.push_back(rec);
    }

    const std::size_t k = std::min(static_cast<std::size_t>(top_k), scored.size());
    std::partial_sort(scored.begin(), scored.begin() + k, scored.end(),
                      [](const Record& a, const Record& b) { return a.score > b.score; });
    out.assign(scored.begin(), scored.begin() + k);
    return out;
}

bool LocalMemory::forget(std::int64_t user_id, std::int64_t id, std::string* err) {
    if (user_id <= 0 || id <= 0) return false;
    try {
        // Soft delete: stamp deleted_at (and updated_at) so the row drops out of
        // recall but stays as history. Scoped to an active row of the caller's, so
        // a wrong owner or an already-forgotten id affects nothing (returns false).
        const std::int64_t now = platform::now_unix_ms();
        database::Result r = db_.execute(
            "UPDATE memories SET deleted_at = ?, updated_at = ? "
            "WHERE id = ? AND user_id = ? AND deleted_at IS NULL",
            { now, now, id, user_id });
        return r.rows_affected > 0;
    } catch (const std::exception& ex) {
        if (err) *err = std::string("memory: delete failed: ") + ex.what();
        return false;
    }
}

//------------------------------------------------------------------------------

std::unique_ptr<Memory> make_local_memory(const Config& cfg, database::Database& db) {
    // Bind the config-driven embedder behind the EmbedFn seam. A copy of `cfg`
    // is not taken -- it outlives the server -- so the lambda captures by
    // reference, matching every other long-lived borrow in the server.
    const Config* cfgp = &cfg;
    EmbedFn embed = [cfgp](const std::string& text, std::string* err) {
        return embedding::text_embedding_one(*cfgp, text, err);
    };
    return std::unique_ptr<Memory>(new LocalMemory(db, cfg.memory.top_k, std::move(embed)));
}

}}
