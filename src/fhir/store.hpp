#pragma once

// DB-backed persistence for FHIR resources, scoped per user.
//
// Resources are stored as generic JSON in a single `fhir_resources` table,
// keyed by (user_id, resource_type, resource_id). The store is a thin
// persistence layer over database::Database — version bumping, meta injection,
// and id assignment live in the REST layer (fhir/rest.cpp), which owns the
// request context. Like the rest of the server, it runs on the single service
// thread and shares the borrowed Database; it does no locking of its own.

#include <string>
#include <vector>

#include <cstdint>

namespace mirobody { namespace database { class Database; } }

namespace mirobody { namespace fhir {

// One persisted resource row. `content` is the full resource JSON (including
// its injected id + meta). version_id starts at 1 and increments per write.
struct StoredResource {
    std::string  type;
    std::string  id;
    std::int64_t version_id = 0;
    std::int64_t updated_at = 0;   // last write, unix ms (app-stamped). The FHIR
                                   // meta.lastUpdated ISO instant lives in `content`;
                                   // this backs search ordering + Last-Modified.
    bool         deleted = false;  // derived: the deleted_at column IS NOT NULL
    std::string  content;          // resource JSON
};

class FhirStore {
public:
    // Borrows `db` (must outlive the store). Creates the fhir_resources table
    // if it does not exist (portable DDL across the linked SQL backend).
    explicit FhirStore(database::Database& db);

    FhirStore(const FhirStore&) = delete;
    FhirStore& operator=(const FhirStore&) = delete;

    // Fetch one resource. Returns false when absent. A soft-deleted row IS
    // returned (out.deleted == true) so the REST layer can answer 410 Gone.
    bool get(std::int64_t user_id, const std::string& type, const std::string& id,
             StoredResource& out);

    // Insert or replace the (user, type, id) row with `r` verbatim. Caller has
    // already computed r.version_id / r.updated_at / r.content.
    void upsert(std::int64_t user_id, const StoredResource& r);

    // Mark (user, type, id) deleted, bumping version and stamping updated_at /
    // deleted_at to `updated_at` (unix ms). Returns false when the row does not
    // exist (REST answers 404).
    bool soft_delete(std::int64_t user_id, const std::string& type, const std::string& id,
                     std::int64_t new_version, std::int64_t updated_at);

    // Page of live (non-deleted) resources of `type` for `user`, newest first.
    // `id_filter` (when non-empty) restricts to that resource id (the FHIR
    // `_id` search param). `total` is set to the unpaged match count.
    std::vector<StoredResource> search(std::int64_t user_id, const std::string& type,
                                       const std::string& id_filter,
                                       int count, int offset, std::int64_t& total);

private:
    database::Database& db_;
};

}}  // namespace mirobody::fhir
