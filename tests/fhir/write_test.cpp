#include "fhir/write.hpp"

#include "database/database.hpp"
#include "fhir/store.hpp"

#include <rapidjson/document.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

// fhir::write_resource is the single place a validated resource becomes a row, and
// three callers depend on it agreeing with itself: POST /fhir/{type} (create),
// PUT /fhir/{type}/{id} (upsert), and the C ABI's on-device health ingest
// (mirobody_health_store). The behaviour under test is what those three actually
// rely on -- id assignment, the version counter, injected meta, and above all that
// writing the SAME id twice REPLACES rather than duplicates, which is what makes a
// phone able to re-sync an overlapping window.
//
// Against a real in-memory SQLite DB, like tests/circle/access_test.cpp.

namespace {

namespace db   = mirobody::database;
namespace fhir = mirobody::fhir;

// Fresh in-memory DB holding just fhir_resources. FhirStore does NOT create it
// (the real table comes from res/sql/sqlite/1_health.sql via apply_schema), so
// the shape below is a copy of that DDL -- keep them in step.
db::Database make_db() {
    db::SQLiteConfig sc;
    sc.path = ":memory:";
    db::Database conn = sc.open();
    conn.execute(
        "CREATE TABLE fhir_resources ("
        " user_id INTEGER NOT NULL,"
        " resource_type TEXT NOT NULL,"
        " resource_id TEXT NOT NULL,"
        " version_id INTEGER NOT NULL DEFAULT 1,"
        " updated_at INTEGER NOT NULL,"
        " deleted_at INTEGER,"
        " content TEXT NOT NULL,"
        " PRIMARY KEY (user_id, resource_type, resource_id));");
    return conn;
}

rapidjson::Document parse(const std::string& json) {
    rapidjson::Document d;
    d.Parse(json.c_str(), json.size());
    REQUIRE_FALSE(d.HasParseError());
    return d;
}

// A minimal steps Observation, shaped like the one the HarmonyOS client writes.
std::string observation(const std::string& id, int steps) {
    std::string body =
        "{\"resourceType\":\"Observation\",\"status\":\"final\","
        "\"code\":{\"coding\":[{\"system\":\"http://loinc.org\",\"code\":\"41950-7\"}]},"
        "\"valueQuantity\":{\"value\":" + std::to_string(steps) + ",\"code\":\"{steps}\"}";
    if (!id.empty()) body += ",\"id\":\"" + id + "\"";
    return body + "}";
}

std::int64_t stored_count(fhir::FhirStore& store, std::int64_t user) {
    std::int64_t total = 0;
    store.search(user, "Observation", std::string(), 50, 0, total);
    return total;
}

}  // namespace

TEST_CASE("write_resource assigns an id and meta on create", "[fhir][write]") {
    db::Database conn = make_db();
    fhir::FhirStore store(conn);

    rapidjson::Document doc = parse(observation("", 8000));
    const fhir::WriteResult w = write_resource(store, 1, "Observation", std::string(), doc);

    REQUIRE_FALSE(w.existed);
    REQUIRE(w.version == 1);
    REQUIRE(w.updated_at > 0);
    REQUIRE(w.id.size() == 36);        // uuid v4, assigned because no id was given
    REQUIRE(w.content.find("\"versionId\":\"1\"") != std::string::npos);
    REQUIRE(w.content.find("\"lastUpdated\":\"") != std::string::npos);
    REQUIRE(w.content.find("\"id\":\"" + w.id + "\"") != std::string::npos);

    // The document itself was rewritten, so a caller that serializes it agrees.
    REQUIRE(doc.HasMember("id"));
    REQUIRE(std::string(doc["id"].GetString()) == w.id);

    fhir::StoredResource got;
    REQUIRE(store.get(1, "Observation", w.id, got));
    REQUIRE_FALSE(got.deleted);
    REQUIRE(got.content == w.content);
}

TEST_CASE("write_resource at a caller-chosen id is idempotent", "[fhir][write]") {
    db::Database conn = make_db();
    fhir::FhirStore store(conn);
    const std::string id = "hw.steps.1754352000000";   // the device-sync id shape

    rapidjson::Document first = parse(observation(id, 8000));
    const fhir::WriteResult a = write_resource(store, 1, "Observation", id, first);
    REQUIRE(a.id == id);
    REQUIRE_FALSE(a.existed);
    REQUIRE(a.version == 1);

    // Re-syncing an overlapping window: same reading, same id, corrected value.
    rapidjson::Document second = parse(observation(id, 8432));
    const fhir::WriteResult b = write_resource(store, 1, "Observation", id, second);
    REQUIRE(b.id == id);
    REQUIRE(b.existed);                 // recognized as an update, not a create
    REQUIRE(b.version == 2);            // and the version counted up
    REQUIRE(b.content.find("\"versionId\":\"2\"") != std::string::npos);
    REQUIRE(b.content.find("8432") != std::string::npos);

    // ONE row, not two -- the whole point of the deterministic id.
    REQUIRE(stored_count(store, 1) == 1);
}

TEST_CASE("write_resource keeps users apart and resurrects a soft delete", "[fhir][write]") {
    db::Database conn = make_db();
    fhir::FhirStore store(conn);
    const std::string id = "hw.hr.1754352060000";

    rapidjson::Document mine = parse(observation(id, 72));
    write_resource(store, 1, "Observation", id, mine);
    rapidjson::Document theirs = parse(observation(id, 61));
    const fhir::WriteResult other = write_resource(store, 2, "Observation", id, theirs);

    // Same id under a different user is a different resource: version restarts and
    // neither row is visible to the other subject.
    REQUIRE_FALSE(other.existed);
    REQUIRE(other.version == 1);
    REQUIRE(stored_count(store, 1) == 1);
    REQUIRE(stored_count(store, 2) == 1);

    // A soft-deleted row is resurrected by a later write, and its version keeps
    // counting: the delete is a version of this resource's history, not a reset.
    REQUIRE(store.soft_delete(1, "Observation", id, 2, 1754352120000LL));
    rapidjson::Document again = parse(observation(id, 70));
    const fhir::WriteResult back = write_resource(store, 1, "Observation", id, again);
    REQUIRE_FALSE(back.existed);        // no LIVE row was there
    REQUIRE(back.version == 3);         // ...but the history says otherwise
    fhir::StoredResource got;
    REQUIRE(store.get(1, "Observation", id, got));
    REQUIRE_FALSE(got.deleted);
}

TEST_CASE("write_resource preserves meta.profile it did not write", "[fhir][write]") {
    db::Database conn = make_db();
    fhir::FhirStore store(conn);

    rapidjson::Document doc = parse(
        "{\"resourceType\":\"Observation\",\"status\":\"final\","
        "\"meta\":{\"profile\":[\"http://example.org/StructureDefinition/steps\"],"
        "\"versionId\":\"99\"}}");
    const fhir::WriteResult w = write_resource(store, 1, "Observation", "keep.meta", doc);

    REQUIRE(w.content.find("example.org/StructureDefinition/steps") != std::string::npos);
    REQUIRE(w.content.find("\"versionId\":\"1\"") != std::string::npos);   // ours wins
    REQUIRE(w.content.find("\"versionId\":\"99\"") == std::string::npos);
}

TEST_CASE("iso8601_instant renders a FHIR instant", "[fhir][write]") {
    // Sub-second precision is dropped (the `instant` granularity the REST layer's
    // Last-Modified header also uses).
    REQUIRE(fhir::iso8601_instant(1785888000000LL) == "2026-08-05T00:00:00Z");
    REQUIRE(fhir::iso8601_instant(1785888000789LL) == "2026-08-05T00:00:00Z");
    REQUIRE(fhir::iso8601_instant(1754352000000LL) == "2025-08-05T00:00:00Z");
}

TEST_CASE("new_resource_id is a distinct uuid v4 each time", "[fhir][write]") {
    const std::string a = fhir::new_resource_id();
    const std::string b = fhir::new_resource_id();
    REQUIRE(a.size() == 36);
    REQUIRE(a[14] == '4');            // version nibble
    REQUIRE(a != b);
}

