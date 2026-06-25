#include "chat/history.hpp"

#include "cache/cache.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using mirobody::cache::Cache;
using mirobody::llm::ChatMessage;
namespace chat = mirobody::chat;

namespace {

std::vector<ChatMessage> exchange(const std::string& q, const std::string& a) {
    std::vector<ChatMessage> turns;
    turns.push_back(ChatMessage{"user", q, {}});
    turns.push_back(ChatMessage{"assistant", a, {}});
    return turns;
}

}   // namespace

TEST_CASE("history round-trips exchanges in order", "[history]") {
    Cache cache;   // in-process backend
    REQUIRE(chat::history_load(cache, 42, "s1").empty());

    chat::history_append(cache, 42, "s1", exchange("q1", "a1"));
    chat::history_append(cache, 42, "s1", exchange("q2", "a2"));

    const std::vector<ChatMessage> got = chat::history_load(cache, 42, "s1");
    REQUIRE(got.size() == 4);
    REQUIRE(got[0].role == "user");
    REQUIRE(got[0].content == "q1");
    REQUIRE(got[1].role == "assistant");
    REQUIRE(got[1].content == "a1");
    REQUIRE(got[3].content == "a2");
}

TEST_CASE("history is scoped per user and per session", "[history]") {
    Cache cache;
    chat::history_append(cache, 1, "s", exchange("mine", "ok"));
    REQUIRE(chat::history_load(cache, 1, "s").size() == 2);
    REQUIRE(chat::history_load(cache, 2, "s").empty());
    REQUIRE(chat::history_load(cache, 1, "other").empty());
}

TEST_CASE("history drops the oldest past the cap", "[history]") {
    Cache cache;
    const std::size_t extra = 2;
    for (std::size_t i = 0; i < (chat::kHistoryMaxMessages / 2) + extra; ++i) {
        chat::history_append(cache, 9, "s",
                             exchange("q" + std::to_string(i), "a" + std::to_string(i)));
    }
    const std::vector<ChatMessage> got = chat::history_load(cache, 9, "s");
    REQUIRE(got.size() == chat::kHistoryMaxMessages);
    REQUIRE(got.front().content == "q" + std::to_string(extra));   // 0..extra-1 fell off
    REQUIRE(got.back().content ==
            "a" + std::to_string(chat::kHistoryMaxMessages / 2 + extra - 1));
}

TEST_CASE("history ignores anonymous / sessionless conversations", "[history]") {
    Cache cache;
    chat::history_append(cache, 0, "s", exchange("x", "y"));   // user_id <= 0
    chat::history_append(cache, 5, "", exchange("x", "y"));    // no session
    REQUIRE(chat::history_load(cache, 0, "s").empty());
    REQUIRE(chat::history_load(cache, 5, "").empty());
    REQUIRE(cache.empty());   // nothing was written at all
}

TEST_CASE("history skips messages without a role", "[history]") {
    Cache cache;
    std::vector<ChatMessage> turns;
    turns.push_back(ChatMessage{"", "stray", {}});
    chat::history_append(cache, 6, "s", turns);
    REQUIRE(chat::history_load(cache, 6, "s").empty());
    turns.push_back(ChatMessage{"user", "kept", {}});
    chat::history_append(cache, 6, "s", turns);
    const std::vector<ChatMessage> got = chat::history_load(cache, 6, "s");
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].content == "kept");
}
