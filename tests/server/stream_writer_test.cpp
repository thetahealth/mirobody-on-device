// StreamWriter's producer side -- in particular the two primitives the SSE
// keepalive pump stands on: knowing how long a stream has been silent, and
// appending to it only while it is still open.
//
// A StreamWriter with a null lws_context never wakes a service loop, which is
// exactly what a test wants: the buffer and the flags are the whole subject.

#include "server/stream_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using mirobody::server::StreamWriter;

namespace {

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}   // namespace

//------------------------------------------------------------------------------

TEST_CASE("idle_ms measures the silence since the last write", "[stream]") {
    StreamWriter w(nullptr);
    w.begin(200, "text/event-stream");

    sleep_ms(60);
    const long long idle = w.idle_ms();
    CHECK(idle >= 50);            // the clock moved...
    CHECK(idle < 5000);           // ...and it is measuring this stream, not the epoch

    w.write("data: {}\n\n");
    CHECK(w.idle_ms() < 50);      // a write resets it
}

TEST_CASE("write_unless_finished appends while the stream is open", "[stream]") {
    StreamWriter w(nullptr);
    w.begin(200, "text/event-stream");

    REQUIRE(w.write_unless_finished(": ping\n\n"));
    std::string out;
    CHECK_FALSE(w.take(out));     // not finished
    CHECK(out == ": ping\n\n");
    CHECK(w.idle_ms() < 50);      // a keepalive counts as activity too
}

TEST_CASE("write_unless_finished refuses once the stream has ended", "[stream]") {
    StreamWriter w(nullptr);
    w.begin(200, "text/event-stream");
    CHECK_FALSE(w.is_finished());

    w.finish("data: {\"type\":\"end\"}\n\n");
    CHECK(w.is_finished());

    // The pump's stop condition: no bytes may follow the terminal chunk, and the
    // false return is how the pump learns the turn is over.
    CHECK_FALSE(w.write_unless_finished(": ping\n\n"));

    std::string out;
    CHECK(w.take(out));
    CHECK(out == "data: {\"type\":\"end\"}\n\n");
}

TEST_CASE("fail() also closes the stream to keepalives", "[stream]") {
    StreamWriter w(nullptr);
    w.begin(200, "text/event-stream");
    w.fail();

    CHECK(w.is_finished());
    CHECK(w.failed());
    CHECK_FALSE(w.write_unless_finished(": ping\n\n"));
}

TEST_CASE("a keepalive racing the end of a turn never lands after it", "[stream]") {
    // The reason the check and the append share one lock. A pump hammering the
    // writer while the turn ends must not slip a comment in behind the terminal
    // chunk -- the router marks that chunk LWS_WRITE_HTTP_FINAL, so anything
    // after it is either dropped or corrupts the framing.
    const std::string kEnd = "data: {\"type\":\"end\"}\n\n";

    for (int round = 0; round < 200; round ++) {
        StreamWriter w(nullptr);
        w.begin(200, "text/event-stream");

        std::atomic<bool> stop(false);
        std::thread pump([&w, &stop]() {
            while (!stop.load()) {
                if (!w.write_unless_finished(": ping\n\n")) return;
            }
        });

        sleep_ms(1);
        w.finish(kEnd);
        stop.store(true);
        pump.join();

        std::string out;
        REQUIRE(w.take(out));
        // Whatever the pump managed to queue, the terminal chunk is the tail.
        REQUIRE(out.size() >= kEnd.size());
        CHECK(out.compare(out.size() - kEnd.size(), kEnd.size(), kEnd) == 0);
        CHECK(out.find(kEnd) == out.size() - kEnd.size());   // and appears once
    }
}
