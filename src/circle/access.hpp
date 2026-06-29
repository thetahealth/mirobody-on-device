#pragma once

// Care-circle health-data access authorization. A tiny seam used by the FHIR
// read/write paths (src/fhir/rest.cpp) so they can serve or modify one circle
// member's records for another without depending on the whole CircleService.

#include <cstdint>
#include <string>
#include <vector>

namespace mirobody { namespace database { class Database; } }

namespace mirobody { namespace circle {

// One person whose health data the viewer may read (via a shared circle), with
// display fields for a picker. `nickname` is the label set on their membership
// (may be empty); `email` falls back when there's no nickname.
//
// `member_id` is the opaque handle to surface to clients (a care_circle_members
// row id) so the real `user_id` (the global users PK) never leaves the server;
// `user_id` stays for in-process gating only (never serialize it to a client).
struct HealthShare {
    std::int64_t member_id = 0;   // opaque handle (care_circle_members.id) for clients
    std::int64_t user_id = 0;     // internal users PK — do NOT send to clients
    std::string  email;
    std::string  nickname;
};

// Everyone (other than `viewer`) who has shared their health data with `viewer`
// through some circle (their health_access >= View, both Accepted). Powers the
// "shared with me" picker and the family_health tool's name resolution. Empty on
// the legacy backend or on error.
std::vector<HealthShare> health_shared_with(database::Database& db, std::int64_t viewer);

// True when `viewer` may read `target`'s health (FHIR) data:
//   - the same user (you always read your own), or
//   - `viewer` and `target` are both Accepted members of a shared care circle
//     AND `target`'s health_access is >= View (care_circle_members.health_access).
// Legacy backend (no care circles): only self. Never throws (false on error).
bool can_read_health(database::Database& db, std::int64_t viewer, std::int64_t target);

// True when `viewer` may write `target`'s health (FHIR) data: the same user, or
// co-Accepted in a shared circle AND `target`'s health_access is Edit. Edit
// implies read, so can_write_health => can_read_health. Same legacy/error rules.
bool can_write_health(database::Database& db, std::int64_t viewer, std::int64_t target);

// Resolve an opaque member handle (`member_id` = care_circle_members.id, as
// surfaced by health_shared_with / the circle routes) to the underlying target
// user_id, BUT only when `viewer` is authorized to reach that member's health
// data through the handle's own circle: both Accepted in it and the member's
// health_access >= View (need_write=false) or Edit (need_write=true). Returns 0
// when the handle is unknown, inactive, or access is not granted. This is the
// opaque-id entry point for FHIR `?subject=` and the chat "currently for"
// subject, so a raw users PK never has to cross the wire. Never throws.
std::int64_t resolve_health_subject(database::Database& db, std::int64_t viewer,
                                    std::int64_t member_id, bool need_write);

}}
