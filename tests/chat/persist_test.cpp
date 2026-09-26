#include "chat/chat.hpp"

#include "config/config.hpp"
#include "database/database.hpp"
#include "database/enums.hpp"   // MessageRole

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

// persist_history's thread logic is exercised against a real (in-memory) DB.
// SQLite-only: it uses the auto-increment / last_insert_id path and is the easy
// backend to stand up with no server (mirrors tests/memory/memory_test.cpp).

namespace {

namespace db = mirobody::database;

// Fresh in-memory DB with the chat tables (the sqlite/1_chat.sql shapes).
db::Database make_chat_db() {
    db::SQLiteConfig sc;
    sc.path = ":memory:";
    db::Database conn = sc.open();
    conn.execute(
        "CREATE TABLE conversations ("
        " id INTEGER NOT NULL PRIMARY KEY,"
        " user_id INTEGER, summary TEXT,"
        " created_at INTEGER NOT NULL, updated_at INTEGER, deleted_at INTEGER);");
    conn.execute(
        "CREATE TABLE messages ("
        " id INTEGER NOT NULL PRIMARY KEY,"
        " user_id INTEGER NOT NULL, conversation_id INTEGER, question_id INTEGER,"
        " role INTEGER NOT NULL, agent TEXT, provider TEXT, content TEXT,"
        " language TEXT, timezone TEXT, files TEXT,"
        " created_at INTEGER NOT NULL, deleted_at INTEGER);");
    return conn;
}

// First column of the first row as an integer, or -1 when none / NULL.
std::int64_t scalar(db::Database& c, const std::string& sql) {
    db::Result r = c.execute(sql);
    if (r.rows.empty() || r.rows[0].empty() || r.rows[0][0].is_null()) return -1;
    return r.rows[0][0].as_int();
}

mirobody::chat::AgentRequest make_req(std::int64_t uid, const std::string& q) {
    mirobody::chat::AgentRequest req;
    req.user_id  = uid;
    req.question = q;
    return req;
}

}   // namespace

//------------------------------------------------------------------------------

TEST_CASE("persist_history starts a new thread and stamps the request ids", "[chat][db]") {
    db::Database conn = make_chat_db();
    mirobody::Config cfg;
    mirobody::chat::Chat chat(cfg, conn);

    mirobody::chat::AgentRequest req = make_req(7, "how did I sleep?");
    const std::int64_t cid = chat.persist_history(req);

    REQUIRE(cid > 0);
    CHECK(req.conversation_id == cid);
    CHECK(req.question_id == cid);   // the opening question's id IS the thread id

    CHECK(scalar(conn, "SELECT count(*) FROM conversations;") == 1);
    CHECK(scalar(conn, "SELECT user_id FROM conversations;") == 7);
    // The root question carries a NULL conversation_id (it is the root).
    CHECK(scalar(conn, "SELECT count(*) FROM messages WHERE conversation_id IS NULL;") == 1);
    CHECK(scalar(conn, "SELECT role FROM messages WHERE id=" + std::to_string(cid) + ";")
          == static_cast<std::int64_t>(db::MessageRole::User));
}

TEST_CASE("persist_history appends a follow-up to an owned thread", "[chat][db]") {
    db::Database conn = make_chat_db();
    mirobody::Config cfg;
    mirobody::chat::Chat chat(cfg, conn);

    mirobody::chat::AgentRequest first = make_req(7, "q1");
    const std::int64_t cid = chat.persist_history(first);

    mirobody::chat::AgentRequest second = make_req(7, "q2");
    second.conversation_id = cid;   // client continues the same thread
    const std::int64_t cid2 = chat.persist_history(second);

    CHECK(cid2 == cid);                 // same thread, not a new one
    CHECK(second.question_id > 0);
    CHECK(second.question_id != cid);   // a distinct follow-up question row

    CHECK(scalar(conn, "SELECT count(*) FROM conversations;") == 1);
    CHECK(scalar(conn, "SELECT count(*) FROM messages;") == 2);
    CHECK(scalar(conn, "SELECT conversation_id FROM messages WHERE id="
                       + std::to_string(second.question_id) + ";") == cid);
    CHECK(scalar(conn, "SELECT updated_at FROM conversations WHERE id="
                       + std::to_string(cid) + ";") > 0);   // thread touched
}

TEST_CASE("persist_history ignores a thread the caller does not own", "[chat][db]") {
    db::Database conn = make_chat_db();
    mirobody::Config cfg;
    mirobody::chat::Chat chat(cfg, conn);

    mirobody::chat::AgentRequest owner = make_req(7, "owner question");
    const std::int64_t cid = chat.persist_history(owner);

    // User 9 claims user 7's thread -> a fresh thread is started instead.
    mirobody::chat::AgentRequest intruder = make_req(9, "intruder question");
    intruder.conversation_id = cid;
    const std::int64_t cid2 = chat.persist_history(intruder);

    CHECK(cid2 != cid);
    CHECK(cid2 > 0);
    CHECK(intruder.conversation_id == cid2);
    CHECK(scalar(conn, "SELECT count(*) FROM conversations;") == 2);
    CHECK(scalar(conn, "SELECT user_id FROM conversations WHERE id="
                       + std::to_string(cid2) + ";") == 9);
    // User 7's thread did NOT gain the intruder's question.
    CHECK(scalar(conn, "SELECT count(*) FROM messages WHERE conversation_id="
                       + std::to_string(cid) + ";") == 0);
}

