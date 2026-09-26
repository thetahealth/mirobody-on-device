#include "memory/local_memory.hpp"

#include "database/database.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>


namespace {

namespace db  = mirobody::database;
namespace mem = mirobody::memory;

// A deterministic stand-in for the real embedder: maps three keywords onto an
// orthonormal basis so cosine similarity is predictable without any network.
// A text with none of the keywords embeds to the zero vector (cosine 0).
std::vector<float> stub_embed(const std::string& text, std::string* /*err*/) {
    std::vector<float> v(3, 0.0f);
    if (text.find("cat")  != std::string::npos) v[0] = 1.0f;
    if (text.find("dog")  != std::string::npos) v[1] = 1.0f;
    if (text.find("fish") != std::string::npos) v[2] = 1.0f;
    return v;
}

// Fresh in-memory DB with the memories table (the sqlite/1_memory.sql shape).
db::Database make_db() {
    db::SQLiteConfig sc;
    sc.path = ":memory:";
    db::Database conn = sc.open();
    conn.execute(
        "CREATE TABLE memories ("
        " id INTEGER NOT NULL PRIMARY KEY,"
        " user_id INTEGER NOT NULL,"
        " kind INTEGER NOT NULL DEFAULT 1,"
        " content TEXT NOT NULL,"
        " embedding BLOB,"
        " conversation_id INTEGER,"
        " created_at INTEGER NOT NULL,"
        " updated_at INTEGER,"
        " deleted_at INTEGER);");
    return conn;
}

}   // namespace

//------------------------------------------------------------------------------

TEST_CASE("LocalMemory recall ranks by cosine similarity", "[memory][db]") {
    db::Database conn = make_db();
    mem::LocalMemory store(conn, 5, &stub_embed);

    std::string err;
    mem::RememberInput in;
    in.user_id = 7;
    in.text = "the user has a cat";   REQUIRE(store.remember(in, &err) > 0);
    in.text = "the user has a dog";   REQUIRE(store.remember(in, &err) > 0);
    in.text = "the user has a fish";  REQUIRE(store.remember(in, &err) > 0);
    REQUIRE(err.empty());

    const std::vector<mem::Record> hits = store.recall(7, "tell me about the cat", 2, &err);
    REQUIRE(err.empty());
    REQUIRE(hits.size() == 2);
    // "cat" memory is the closest match (cosine 1), so it ranks first.
    REQUIRE(hits[0].text == "the user has a cat");
    REQUIRE(hits[0].score > hits[1].score);
}

//------------------------------------------------------------------------------

TEST_CASE("LocalMemory recall is scoped to the owning user", "[memory][db]") {
    db::Database conn = make_db();
    mem::LocalMemory store(conn, 5, &stub_embed);

    std::string err;
    mem::RememberInput a; a.user_id = 1; a.text = "user one likes cat";  store.remember(a, &err);
    mem::RememberInput b; b.user_id = 2; b.text = "user two likes dog";  store.remember(b, &err);

    const std::vector<mem::Record> for_one = store.recall(1, "cat or dog", 10, &err);
    REQUIRE(for_one.size() == 1);
    REQUIRE(for_one[0].text == "user one likes cat");

    // A user with no memories recalls nothing.
    REQUIRE(store.recall(99, "cat", 10, &err).empty());
}

//------------------------------------------------------------------------------

TEST_CASE("LocalMemory blank text / query are no-ops", "[memory][db]") {
    db::Database conn = make_db();
    mem::LocalMemory store(conn, 5, &stub_embed);

    std::string err;
    mem::RememberInput in; in.user_id = 5; in.text = "   ";
    REQUIRE(store.remember(in, &err) == 0);   // blank -> no-op success
    REQUIRE(err.empty());

    in.text = "user likes fish";
    REQUIRE(store.remember(in, &err) > 0);
    REQUIRE(store.recall(5, "   ", 5, &err).empty());   // blank query -> empty
    REQUIRE(err.empty());
}

//------------------------------------------------------------------------------

TEST_CASE("LocalMemory forget removes only the owner's row", "[memory][db]") {
    db::Database conn = make_db();
    mem::LocalMemory store(conn, 5, &stub_embed);

    std::string err;
    mem::RememberInput in; in.user_id = 3; in.text = "user has a cat";
    const std::int64_t id = store.remember(in, &err);
    REQUIRE(id > 0);

    REQUIRE_FALSE(store.forget(999, id, &err));   // wrong owner: nothing removed
    REQUIRE(store.recall(3, "cat", 5, &err).size() == 1);

    REQUIRE(store.forget(3, id, &err));           // owner: removed
    REQUIRE(store.recall(3, "cat", 5, &err).empty());
}

