#include "circle/access.hpp"

#include "database/database.hpp"
#include "database/enums.hpp"   // CircleStatus, ShareAccess

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

// The cross-user health-data authorization seam (circle::can_read_health /
// can_write_health / health_shared_with) is the one place the otherwise
// strictly single-user core lets one user reach another's records, so it gets
// direct coverage. Exercised against a real in-memory SQLite DB (the easy
// backend to stand up with no server; mirrors tests/chat/persist_test.cpp).

namespace {

namespace db     = mirobody::database;
namespace circle = mirobody::circle;

const int kPending  = static_cast<int>(db::CircleStatus::Pending);
const int kAccepted = static_cast<int>(db::CircleStatus::Accepted);
const int kOff      = static_cast<int>(db::ShareAccess::Unknown);   // 0 = off
const int kView     = static_cast<int>(db::ShareAccess::View);
const int kEdit     = static_cast<int>(db::ShareAccess::Edit);

// Fresh in-memory DB with the two tables the access seam touches (the
// care_circle_members shape from sqlite/2_care_circle.sql, plus users for the
// email/nickname join in health_shared_with).
db::Database make_circle_db() {
    db::SQLiteConfig sc;
    sc.path = ":memory:";
    db::Database conn = sc.open();
    conn.execute(
        "CREATE TABLE care_circle_members ("
        " id INTEGER NOT NULL PRIMARY KEY,"
        " care_circle_id INTEGER NOT NULL,"
        " user_id INTEGER NOT NULL,"
        " role INTEGER NOT NULL DEFAULT 0,"
        " status INTEGER NOT NULL,"
        " member_email TEXT, invite_token TEXT, nickname TEXT,"
        " health_access INTEGER NOT NULL DEFAULT 0,"
        " created_at INTEGER NOT NULL DEFAULT 0, updated_at INTEGER, deleted_at INTEGER);");
    conn.execute(
        "CREATE TABLE users (id INTEGER NOT NULL PRIMARY KEY, email TEXT);");
    return conn;
}

// Add one (circle, user) membership row. Active rows leave deleted_at out
// entirely so it stays NULL (binding "" would store an empty string, which
// `deleted_at IS NULL` would not match); deleted rows stamp it.
void add_member(db::Database& c, std::int64_t circle, std::int64_t user,
                int status, int health, const std::string& nickname = "",
                bool deleted = false) {
    if (deleted) {
        c.execute(
            "INSERT INTO care_circle_members "
            "(care_circle_id, user_id, status, health_access, nickname, deleted_at) "
            "VALUES (?, ?, ?, ?, ?, ?);",
            {circle, user, status, health, nickname, std::int64_t(1)});
    } else {
        c.execute(
            "INSERT INTO care_circle_members "
            "(care_circle_id, user_id, status, health_access, nickname) "
            "VALUES (?, ?, ?, ?, ?);",
            {circle, user, status, health, nickname});
    }
}

void add_user(db::Database& c, std::int64_t id, const std::string& email) {
    c.execute("INSERT INTO users (id, email) VALUES (?, ?);", {id, email});
}

// First column of the first row as an integer, or -1 when none / NULL.
std::int64_t scalar(db::Database& c, const std::string& sql) {
    db::Result r = c.execute(sql);
    if (r.rows.empty() || r.rows[0].empty() || r.rows[0][0].is_null()) return -1;
    return r.rows[0][0].as_int();
}

}   // namespace

//------------------------------------------------------------------------------

TEST_CASE("you can always read and write your own health data", "[circle][db]") {
    db::Database c = make_circle_db();
    // No membership rows at all: self-access must still hold.
    CHECK(circle::can_read_health(c, 7, 7));
    CHECK(circle::can_write_health(c, 7, 7));
}

TEST_CASE("an unauthenticated viewer is never authorized", "[circle][db]") {
    db::Database c = make_circle_db();
    CHECK_FALSE(circle::can_read_health(c, 0, 7));
    CHECK_FALSE(circle::can_read_health(c, -1, 7));
    CHECK_FALSE(circle::can_write_health(c, 0, 7));
}

TEST_CASE("View grants read but not write to a fellow accepted member", "[circle][db]") {
    db::Database c = make_circle_db();
    add_member(c, /*circle*/1, /*viewer*/10, kAccepted, kOff);
    add_member(c, /*circle*/1, /*target*/20, kAccepted, kView);

    CHECK(circle::can_read_health(c, 10, 20));
    CHECK_FALSE(circle::can_write_health(c, 10, 20));   // View != Edit
}

TEST_CASE("Edit grants both read and write", "[circle][db]") {
    db::Database c = make_circle_db();
    add_member(c, 1, 10, kAccepted, kOff);
    add_member(c, 1, 20, kAccepted, kEdit);

    CHECK(circle::can_read_health(c, 10, 20));
    CHECK(circle::can_write_health(c, 10, 20));
}

TEST_CASE("sharing off denies access even between accepted members", "[circle][db]") {
    db::Database c = make_circle_db();
    add_member(c, 1, 10, kAccepted, kOff);
    add_member(c, 1, 20, kAccepted, kOff);

    CHECK_FALSE(circle::can_read_health(c, 10, 20));
    CHECK_FALSE(circle::can_write_health(c, 10, 20));
}

TEST_CASE("a pending membership grants nothing", "[circle][db]") {
    db::Database c = make_circle_db();

    SECTION("target still pending") {
        add_member(c, 1, 10, kAccepted, kOff);
        add_member(c, 1, 20, kPending,  kEdit);   // not yet accepted
        CHECK_FALSE(circle::can_read_health(c, 10, 20));
    }
    SECTION("viewer still pending") {
        add_member(c, 1, 10, kPending,  kOff);
        add_member(c, 1, 20, kAccepted, kEdit);
        CHECK_FALSE(circle::can_read_health(c, 10, 20));
    }
}

TEST_CASE("a soft-deleted membership grants nothing", "[circle][db]") {
    db::Database c = make_circle_db();
    add_member(c, 1, 10, kAccepted, kOff);
    add_member(c, 1, 20, kAccepted, kEdit, "", /*deleted*/true);

    CHECK_FALSE(circle::can_read_health(c, 10, 20));
    CHECK_FALSE(circle::can_write_health(c, 10, 20));
}

TEST_CASE("members of different circles cannot reach each other", "[circle][db]") {
    db::Database c = make_circle_db();
    add_member(c, /*circle*/1, 10, kAccepted, kView);
    add_member(c, /*circle*/2, 20, kAccepted, kEdit);   // a different circle

    CHECK_FALSE(circle::can_read_health(c, 10, 20));
    CHECK_FALSE(circle::can_read_health(c, 20, 10));
}

TEST_CASE("co-membership in any one shared circle is enough", "[circle][db]") {
    db::Database c = make_circle_db();
    // 10 and 20 share circle 2 (where 20 shares View); they are strangers in 1.
    add_member(c, 1, 10, kAccepted, kOff);
    add_member(c, 2, 10, kAccepted, kOff);
    add_member(c, 2, 20, kAccepted, kView);

    CHECK(circle::can_read_health(c, 10, 20));
}

TEST_CASE("health_shared_with lists sharers and excludes self / off", "[circle][db]") {
    db::Database c = make_circle_db();
    add_user(c, 10, "viewer@example.com");
    add_user(c, 20, "mom@example.com");
    add_user(c, 30, "dad@example.com");
    add_user(c, 40, "coworker@example.com");

    add_member(c, 1, 10, kAccepted, kView);              // viewer (self) — excluded
    add_member(c, 1, 20, kAccepted, kEdit, "Mom");       // shares -> listed
    add_member(c, 1, 30, kAccepted, kOff);               // off -> excluded
    add_member(c, 2, 40, kAccepted, kView);              // different circle, not with 10

    std::vector<circle::HealthShare> shares = circle::health_shared_with(c, 10);

    REQUIRE(shares.size() == 1);
    CHECK(shares[0].user_id == 20);
    CHECK(shares[0].nickname == "Mom");
    CHECK(shares[0].email == "mom@example.com");
    CHECK(shares[0].member_id > 0);   // an opaque handle is surfaced, not the users PK
}

TEST_CASE("resolve_health_subject maps an opaque handle to the target under access", "[circle][db]") {
    db::Database c = make_circle_db();
    add_member(c, 1, 10, kAccepted, kOff);    // viewer
    add_member(c, 1, 20, kAccepted, kView);   // target shares View (not Edit)
    const std::int64_t handle = scalar(c, "SELECT id FROM care_circle_members WHERE user_id=20;");

    CHECK(circle::resolve_health_subject(c, 10, handle, /*need_write*/false) == 20);  // read ok
    CHECK(circle::resolve_health_subject(c, 10, handle, /*need_write*/true)  == 0);   // View != Edit
    CHECK(circle::resolve_health_subject(c, 999, handle, false) == 0);                // not a co-member
    CHECK(circle::resolve_health_subject(c, 10, 999999, false) == 0);                 // unknown handle
    CHECK(circle::resolve_health_subject(c, 10, 0, false) == 0);                      // no handle
}

TEST_CASE("resolve_health_subject requires the target to still be sharing", "[circle][db]") {
    db::Database c = make_circle_db();
    add_member(c, 1, 10, kAccepted, kOff);
    add_member(c, 1, 20, kAccepted, kOff);    // target sharing OFF
    const std::int64_t handle = scalar(c, "SELECT id FROM care_circle_members WHERE user_id=20;");
    CHECK(circle::resolve_health_subject(c, 10, handle, false) == 0);
}

