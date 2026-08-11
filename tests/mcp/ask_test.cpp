// AskBroker — the rendezvous that parks a turn inside a tool call until the user
// answers (see src/mcp/ask.hpp).
//
// Worth its own tests because every failure mode here is a HANG or a lost answer,
// neither of which shows up as a wrong value: a waiter that misses its wake-up
// leaves a turn stuck until the timeout, and an answer delivered to the wrong slot
// would put one question's reply into another's tool result.

#include <catch2/catch_test_macros.hpp>

#include "mcp/ask.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using mirobody::mcp::AskBroker;

namespace {

// expect() and wait() must happen on ONE thread (they pair through a thread-local),
// and the answer must come from another — which is exactly how a turn thread and a
// UI thread relate. Every case here runs the waiter on its own thread for that
// reason, not merely to avoid blocking the test.
struct Waiter {
    std::thread      thread;
    std::string      result;
    std::atomic<bool> finished{false};

    Waiter(const std::string& id, int timeout_seconds) {
        thread = std::thread([this, id, timeout_seconds]() {
            AskBroker::instance().expect(id);
            result = AskBroker::instance().wait(timeout_seconds);
            finished.store(true);
        });
    }

    // Give the waiter a moment to reach wait(); answering before it parks is a real
    // race the broker must survive, and settle() covers the ordinary path.
    void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }

    void join() { thread.join(); }
};

}   // namespace

TEST_CASE("AskBroker delivers an answer to the waiting turn", "[mcp][ask]") {
    Waiter w("ask-1", 5);
    w.settle();

    REQUIRE(AskBroker::instance().answer("ask-1", "{\"selected\":[\"A\"]}"));
    w.join();

    REQUIRE(w.finished.load());
    REQUIRE(w.result == "{\"selected\":[\"A\"]}");
}

TEST_CASE("AskBroker drops an answer nobody is waiting for", "[mcp][ask]") {
    // A stale tap — the turn was stopped, or the tool already timed out. It must not
    // be queued for whatever asks next.
    REQUIRE_FALSE(AskBroker::instance().answer("ask-nobody", "{\"selected\":[\"A\"]}"));
}

TEST_CASE("AskBroker takes only the first answer", "[mcp][ask]") {
    Waiter w("ask-2", 5);
    w.settle();

    REQUIRE(AskBroker::instance().answer("ask-2", "{\"selected\":[\"first\"]}"));
    REQUIRE_FALSE(AskBroker::instance().answer("ask-2", "{\"selected\":[\"second\"]}"));
    w.join();

    REQUIRE(w.result == "{\"selected\":[\"first\"]}");
}

TEST_CASE("AskBroker routes by id", "[mcp][ask]") {
    Waiter a("ask-a", 5);
    Waiter b("ask-b", 5);
    a.settle();

    REQUIRE(AskBroker::instance().answer("ask-b", "{\"selected\":[\"for-b\"]}"));
    REQUIRE(AskBroker::instance().answer("ask-a", "{\"selected\":[\"for-a\"]}"));
    a.join();
    b.join();

    REQUIRE(a.result == "{\"selected\":[\"for-a\"]}");
    REQUIRE(b.result == "{\"selected\":[\"for-b\"]}");
}

TEST_CASE("AskBroker cancel_all releases every waiter with no answer", "[mcp][ask]") {
    Waiter a("ask-c1", 30);
    Waiter b("ask-c2", 30);
    a.settle();

    AskBroker::instance().cancel_all();
    a.join();
    b.join();

    // Empty, not an error: the tool turns this into "the user did not answer" so the
    // model can proceed on a default rather than apologize for a failure.
    REQUIRE(a.result.empty());
    REQUIRE(b.result.empty());
}

TEST_CASE("AskBroker gives up when nobody answers", "[mcp][ask]") {
    // The timeout is what guarantees a turn always finishes, even against a client
    // that ignores the event entirely.
    Waiter w("ask-timeout", 1);
    w.join();
    REQUIRE(w.result.empty());
}

TEST_CASE("AskBroker wait without expect returns immediately", "[mcp][ask]") {
    // A tool that runs without the filter having registered anything (ask_user
    // called through a path with no AskFilter) must not park the turn forever.
    std::string out = "unset";
    std::thread t([&out]() { out = AskBroker::instance().wait(30); });
    t.join();
    REQUIRE(out.empty());
}
