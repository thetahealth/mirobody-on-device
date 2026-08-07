#pragma once

// How a validated FHIR resource becomes a row: the server-owned write
// bookkeeping every write path shares.
//
// Three write paths exist and they must agree on the id, the version, and the
// injected meta, or the same resource read back through a different door looks
// like a different resource:
//   - the REST layer's POST /fhir/{type} (create, server-assigned id) and
//     PUT /fhir/{type}/{id} (upsert at a client-chosen id) -- fhir/rest.cpp;
//   - the C ABI's on-device health ingest (mirobody_health_store), which has no
//     HTTP layer at all -- src/platform/c_api.cpp. It writes at a client-chosen
//     id on purpose: a phone re-syncing the same week must replace its readings,
//     not duplicate them, and (user, type, id) is the store's primary key.
//
// PARSING AND VALIDATION STAY WITH THE CALLER (fhir/resource.hpp's
// validate_resource): the REST layer turns issues into an OperationOutcome with
// per-issue status codes, the C ABI collapses them into one error string, and
// folding either policy in here would force the other to unpick it.

#include "fhir/store.hpp"

#include <rapidjson/document.h>

#include <cstdint>
#include <string>

namespace mirobody { namespace fhir {

// What one write did. `existed` distinguishes an update from a create (the REST
// layer answers 200 vs 201 on it); `content` is what was persisted -- the
// caller's resource with the id + meta below injected.
struct WriteResult {
    std::string  id;
    std::int64_t version    = 1;
    std::int64_t updated_at = 0;      // unix ms, as stored in fhir_resources
    bool         existed    = false;  // a live row was already there
    std::string  content;             // the stored resource JSON
};

// Persist `resource` for `user_id`. `id` empty => a fresh server-assigned id
// (create semantics); non-empty => that id, with the version bumped from any
// existing row (upsert semantics). `resource` is REWRITTEN in place with the
// final id and meta, so a caller that also serializes it sees what was stored.
//
// A soft-deleted row at the same id is resurrected: its version keeps counting
// up, matching PUT-after-DELETE in the REST layer.
WriteResult write_resource(FhirStore& store, std::int64_t user_id,
                           const std::string& type, const std::string& id,
                           rapidjson::Document& resource);

// RFC-4122 v4 UUID -- the shape of a server-assigned resource id.
std::string new_resource_id();

// `unix_ms` as a FHIR `instant` ("2026-06-08T11:18:06Z"): meta.lastUpdated, and
// the REST layer's Last-Modified header. Seconds resolution, per the primitive.
std::string iso8601_instant(std::int64_t unix_ms);

// Inject the server-owned id + meta (versionId, lastUpdated) into a parsed
// resource, preserving any meta.profile / security / tag it already carries.
// Takes the document root because it needs its allocator.
void inject_meta(rapidjson::Document& resource, const std::string& id,
                 std::int64_t version, const std::string& last_updated);

}}  // namespace mirobody::fhir
