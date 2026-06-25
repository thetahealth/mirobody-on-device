#include "llm/sse_parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using mirobody::llm::SseParser;

namespace {

struct Collector {
    std::vector<std::string> data;
    void operator()(const void* payload, std::size_t length) {
        data.emplace_back(static_cast<const char*>(payload), length);
    }
};

// Feed a whole string with its exact length (SSE chunks aren't null-terminated).
void feed(SseParser& p, const std::string& s, Collector& c) {
    p.feed(s.data(), s.size(), std::ref(c));
}

}

//------------------------------------------------------------------------------

TEST_CASE("emits one event per blank-line-terminated block", "[sse]") {
    SseParser p;
    Collector got;
    feed(p, "data: hello\n\ndata: world\n\n", got);
    REQUIRE(got.data == std::vector<std::string>{"hello", "world"});
}

//------------------------------------------------------------------------------

TEST_CASE("reassembles events split across feed() calls", "[sse]") {
    SseParser p;
    Collector got;
    // libcurl chunks rarely align with SSE event boundaries; the parser
    // has to buffer partial events between calls.
    feed(p, "data: he", got);
    feed(p, "llo, ", got);
    feed(p, "world\n", got);
    REQUIRE(got.data.empty());
    feed(p, "\ndata: next\n\n", got);
    REQUIRE(got.data == std::vector<std::string>{"hello, world", "next"});
}

//------------------------------------------------------------------------------

TEST_CASE("handles CRLF line endings", "[sse]") {
    SseParser p;
    Collector got;
    feed(p, "data: a\r\n\r\ndata: b\r\n\r\n", got);
    REQUIRE(got.data == std::vector<std::string>{"a", "b"});
}

//------------------------------------------------------------------------------

TEST_CASE("concatenates multi-line data fields with newline", "[sse]") {
    SseParser p;
    Collector got;
    feed(p, "data: line1\ndata: line2\n\n", got);
    REQUIRE(got.data == std::vector<std::string>{"line1\nline2"});
}

//------------------------------------------------------------------------------

TEST_CASE("ignores comment lines and non-data fields", "[sse]") {
    SseParser p;
    Collector got;
    // `:` lines are comments; `event:` / `id:` fields are not consumed here.
    feed(p, ": keep-alive\nevent: ping\nid: 7\ndata: payload\n\n", got);
    REQUIRE(got.data == std::vector<std::string>{"payload"});
}

//------------------------------------------------------------------------------

TEST_CASE("strips a single leading space after data:", "[sse]") {
    SseParser p;
    Collector got;
    feed(p, "data: with-space\n\ndata:no-space\n\ndata:  two-spaces\n\n", got);
    REQUIRE(got.data == std::vector<std::string>{"with-space", "no-space", " two-spaces"});
}

//------------------------------------------------------------------------------

TEST_CASE("finish() flushes a trailing event without a blank-line terminator", "[sse]") {
    SseParser p;
    Collector got;
    feed(p, "data: tail", got);
    REQUIRE(got.data.empty());
    p.finish(std::ref(got));
    REQUIRE(got.data == std::vector<std::string>{"tail"});
}

//------------------------------------------------------------------------------

TEST_CASE("passes [DONE] sentinel through verbatim", "[sse]") {
    SseParser p;
    Collector got;
    feed(p, "data: chunk\n\ndata: [DONE]\n\n", got);
    REQUIRE(got.data == std::vector<std::string>{"chunk", "[DONE]"});
}
