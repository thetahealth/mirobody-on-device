// Care-circle health-data access authorization (declared in circle/access.hpp).
//
// Split out of circle/service.cpp so the authorization seam can be linked
// without the HTTP front door. service.cpp includes circle/service.hpp, which
// includes server/router.hpp and so pulls in libwebsockets; these four
// functions need nothing but the database, yet the FHIR read/write paths and
// the family_health MCP tool depend on them. Keeping them here means a build
// with MIROBODY_MOBILE=ON (embedded C ABI hosts -- see the
// HarmonyOS bridge) still resolves them.

#include "circle/access.hpp"

#include "database/database.hpp"
#include "database/enums.hpp"   // CircleStatus, ShareAccess

#include <exception>
#include <string>
#include <vector>

namespace mirobody { namespace circle {

namespace db = database;

namespace {

const int kAccepted = static_cast<int>(db::CircleStatus::Accepted);
const int kView     = static_cast<int>(db::ShareAccess::View);
const int kEdit     = static_cast<int>(db::ShareAccess::Edit);

// Does `target` grant `viewer` health access of at least `min_level`
// (ShareAccess) through some shared circle? Both must be Accepted, active
// members of the same circle and target.health_access >= min_level. Caller
// handles the self / legacy cases. Never throws (false on error).
bool circle_health_grants(database::Database& conn, std::int64_t viewer,
                          std::int64_t target, int min_level) {
    try {
        database::Result r = conn.execute(
            "SELECT 1 FROM care_circle_members me "
            "JOIN care_circle_members owner ON me.care_circle_id=owner.care_circle_id "
            "WHERE me.user_id=? AND owner.user_id=? AND me.status=? AND owner.status=? "
            "AND owner.health_access >= ? AND me.deleted_at IS NULL AND owner.deleted_at IS NULL LIMIT 1;",
            {std::to_string(viewer), std::to_string(target), kAccepted, kAccepted, min_level});
        return !r.rows.empty() && !r.rows[0].empty();
    } catch (const std::exception&) {
        return false;
    }
}

}   // namespace

//------------------------------------------------------------------------------

bool can_read_health(database::Database& conn, std::int64_t viewer, std::int64_t target) {
    if (viewer <= 0) return false;
    if (viewer == target) return true;   // you always read your own
#if defined(MIROBODY_DATABASE_PG_LEGACY)
    (void)conn;
    return false;                        // no care circles on the legacy backend
#else
    return circle_health_grants(conn, viewer, target, kView);
#endif
}

bool can_write_health(database::Database& conn, std::int64_t viewer, std::int64_t target) {
    if (viewer <= 0) return false;
    if (viewer == target) return true;   // you always write your own
#if defined(MIROBODY_DATABASE_PG_LEGACY)
    (void)conn;
    return false;
#else
    return circle_health_grants(conn, viewer, target, kEdit);
#endif
}

std::int64_t resolve_health_subject(database::Database& conn, std::int64_t viewer,
                                    std::int64_t member_id, bool need_write) {
    if (viewer <= 0 || member_id <= 0) return 0;
#if defined(MIROBODY_DATABASE_PG_LEGACY)
    (void)conn; (void)need_write;
    return 0;                            // no care circles on the legacy backend
#else
    try {
        // The handle names one membership row (owner); authorize the viewer
        // through that SAME circle: both Accepted, and the owner's health_access
        // meets the needed level. Returns the owner's user_id only when allowed.
        database::Result r = conn.execute(
            "SELECT owner.user_id FROM care_circle_members owner "
            "JOIN care_circle_members me ON me.care_circle_id=owner.care_circle_id "
            "WHERE owner.id=? AND me.user_id=? AND owner.status=? AND me.status=? "
            "AND owner.health_access >= ? AND owner.deleted_at IS NULL AND me.deleted_at IS NULL "
            "LIMIT 1;",
            {std::to_string(member_id), std::to_string(viewer),
             kAccepted, kAccepted, need_write ? kEdit : kView});
        if (r.rows.empty() || r.rows[0].empty() || r.rows[0][0].is_null()) return 0;
        return r.rows[0][0].as_int();
    } catch (const std::exception&) {
        return 0;
    }
#endif
}

std::vector<HealthShare> health_shared_with(database::Database& conn, std::int64_t viewer) {
    std::vector<HealthShare> out;
    if (viewer <= 0) return out;
#if defined(MIROBODY_DATABASE_PG_LEGACY)
    (void)conn;
    return out;
#else
    try {
        const std::string v = std::to_string(viewer);
        // MAX(owner.id) yields one stable membership-row handle per sharer (a
        // person sharing through several circles collapses to one entry); that
        // handle round-trips through resolve_health_subject without ever exposing
        // the sharer's users PK.
        database::Result r = conn.execute(
            "SELECT owner.user_id, MAX(u.email), MAX(owner.nickname), MAX(owner.id) "
            "FROM care_circle_members me "
            "JOIN care_circle_members owner ON me.care_circle_id=owner.care_circle_id "
            "LEFT JOIN users u ON u.id=owner.user_id "
            "WHERE me.user_id=? AND owner.user_id<>? AND me.status=? AND owner.status=? "
            "AND owner.health_access>=? AND me.deleted_at IS NULL AND owner.deleted_at IS NULL "
            "GROUP BY owner.user_id;",
            {v, v, kAccepted, kAccepted, kView});
        for (std::size_t i = 0; i < r.rows.size(); ++i) {
            const std::vector<database::Value>& row = r.rows[i];
            if (row.empty() || row[0].is_null()) continue;
            HealthShare h;
            h.user_id   = row[0].as_int();
            h.email     = (row.size() > 1 && !row[1].is_null()) ? row[1].as_text() : std::string();
            h.nickname  = (row.size() > 2 && !row[2].is_null()) ? row[2].as_text() : std::string();
            h.member_id = (row.size() > 3 && !row[3].is_null()) ? row[3].as_int()  : 0;
            out.push_back(h);
        }
    } catch (const std::exception&) {
    }
    return out;
#endif
}

}}
