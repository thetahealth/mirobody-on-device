#include "fhir/store.hpp"

#include "database/database.hpp"

namespace mirobody { namespace fhir {

using database::Value;

FhirStore::FhirStore(database::Database& db) : db_(db) {
}

bool FhirStore::get(std::int64_t user_id, const std::string& type, const std::string& id,
                    StoredResource& out) {
    database::Result r = db_.execute(
        "SELECT version_id, updated_at, deleted_at, content FROM fhir_resources"
        " WHERE user_id = ? AND resource_type = ? AND resource_id = ?",
        {Value(user_id), Value(type), Value(id)});
    if (r.rows.empty()) return false;
    const std::vector<Value>& row = r.rows[0];
    out.type = type;
    out.id = id;
    out.version_id = row[0].as_int();
    out.updated_at = row[1].as_int();
    out.deleted = !row[2].is_null();   // deleted_at set => soft-deleted
    out.content = row[3].as_text();
    return true;
}

void FhirStore::upsert(std::int64_t user_id, const StoredResource& r) {
    // No portable UPSERT across all five dialects, so delete-then-insert. Single
    // service thread, so there is no interleaving write to race with.
    db_.execute(
        "DELETE FROM fhir_resources"
        " WHERE user_id = ? AND resource_type = ? AND resource_id = ?",
        {Value(user_id), Value(r.type), Value(r.id)});
    db_.execute(
        "INSERT INTO fhir_resources"
        " (user_id, resource_type, resource_id, version_id, updated_at, deleted_at, content)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)",
        {Value(user_id), Value(r.type), Value(r.id), Value(r.version_id),
         Value(r.updated_at), (r.deleted ? Value(r.updated_at) : Value(nullptr)), Value(r.content)});
}

bool FhirStore::soft_delete(std::int64_t user_id, const std::string& type,
                            const std::string& id, std::int64_t new_version,
                            std::int64_t updated_at) {
    database::Result r = db_.execute(
        "UPDATE fhir_resources SET deleted_at = ?, version_id = ?, updated_at = ?"
        " WHERE user_id = ? AND resource_type = ? AND resource_id = ? AND deleted_at IS NULL",
        {Value(updated_at), Value(new_version), Value(updated_at),
         Value(user_id), Value(type), Value(id)});
    return r.rows_affected > 0;
}

std::vector<StoredResource> FhirStore::search(std::int64_t user_id, const std::string& type,
                                              const std::string& id_filter,
                                              int count, int offset, std::int64_t& total) {
    std::vector<StoredResource> out;
    total = 0;

    // Count + page share the same WHERE. id_filter is an optional extra clause.
    const bool by_id = !id_filter.empty();
    std::string where = " WHERE user_id = ? AND resource_type = ? AND deleted_at IS NULL";
    if (by_id) where += " AND resource_id = ?";

    std::vector<Value> count_params;
    count_params.push_back(Value(user_id));
    count_params.push_back(Value(type));
    if (by_id) count_params.push_back(Value(id_filter));

    database::Result cr = db_.execute("SELECT COUNT(*) FROM fhir_resources" + where, count_params);
    if (!cr.rows.empty()) total = cr.rows[0][0].as_int();

    std::vector<Value> page_params = count_params;
    page_params.push_back(Value(count));
    page_params.push_back(Value(offset));

    database::Result pr = db_.execute(
        "SELECT resource_id, version_id, updated_at, content FROM fhir_resources" + where +
        " ORDER BY updated_at DESC, resource_id ASC LIMIT ? OFFSET ?",
        page_params);
    out.reserve(pr.rows.size());
    for (size_t i = 0; i < pr.rows.size(); ++i) {
        const std::vector<Value>& row = pr.rows[i];
        StoredResource sr;
        sr.type = type;
        sr.id = row[0].as_text();
        sr.version_id = row[1].as_int();
        sr.updated_at = row[2].as_int();
        sr.deleted = false;
        sr.content = row[3].as_text();
        out.push_back(sr);
    }
    return out;
}

}}  // namespace mirobody::fhir
