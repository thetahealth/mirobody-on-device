#include "client/http_client.hpp"
#include "llm/mirothinker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using mirobody::llm::ChatMessage;
using mirobody::llm::Event;
using mirobody::llm::EventType;
using mirobody::llm::MiroThinkerClient;
using mirobody::llm::MiroThinkerOptions;

namespace {

// libcurl needs a global init before the first curl_easy_init. The validation
// tests below exit early before touching curl, but the live test does call out,
// so initialize once at TU load — costs ~nothing for the validation cases.
struct CurlInit {
    CurlInit()  { mirobody::client::HttpClient::global_init(); }
    ~CurlInit() { mirobody::client::HttpClient::global_cleanup(); }
};
CurlInit g_curl_init;

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string{v} : std::string{};
}

auto collect_into(std::vector<Event>& sink) {
    return [&sink](const Event& e) { sink.push_back(e); return true; };
}

}

//------------------------------------------------------------------------------

TEST_CASE("missing api_key short-circuits with an Error event", "[miro-thinker]") {
    MiroThinkerOptions opt;
    opt.mcp_url = "https://example.com/mcp/abc";  // satisfy the other guard
    MiroThinkerClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
    REQUIRE(events[0].content.find("MIROTHINKER_API_KEY") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("missing mcp_url short-circuits with an Error event", "[miro-thinker]") {
    MiroThinkerOptions opt;
    opt.api_key = "sk-fake";
    MiroThinkerClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
    REQUIRE(events[0].content.find("MCP_PUBLIC_URL") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("empty messages short-circuits with an Error event", "[miro-thinker]") {
    MiroThinkerOptions opt;
    opt.api_key = "sk-fake";
    opt.mcp_url = "https://example.com/mcp/abc";
    MiroThinkerClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
}

//------------------------------------------------------------------------------

TEST_CASE("EventType::to_string covers every variant", "[miro-thinker]") {
    using mirobody::llm::to_string;
    REQUIRE(std::string{to_string(EventType::Reply)}          == "reply");
    REQUIRE(std::string{to_string(EventType::Thinking)}       == "thinking");
    REQUIRE(std::string{to_string(EventType::QueryTitle)}     == "queryTitle");
    REQUIRE(std::string{to_string(EventType::QueryArguments)} == "queryArguments");
    REQUIRE(std::string{to_string(EventType::QueryDetail)}    == "queryDetail");
    REQUIRE(std::string{to_string(EventType::CostStatistics)} == "costStatistics");
    REQUIRE(std::string{to_string(EventType::Error)}          == "error");
}

//------------------------------------------------------------------------------
// Live integration test against api.miromind.ai. Hidden by Catch2's `[.]`
// tag — won't run unless explicitly requested:
//
//   mirobody_tests "[miro-thinker-live]"
//
// Set MIROTHINKER_API_KEY and MCP_PUBLIC_URL in env first; the test skips
// with a clear message if either is unset.
//------------------------------------------------------------------------------

TEST_CASE("live: simple round trip yields content + CostStatistics", "[miro-thinker-live][.]") {
    std::string api_key = env_or_empty("MIROTHINKER_API_KEY");
    std::string mcp_url = env_or_empty("MCP_PUBLIC_URL");
    if (api_key.empty() || mcp_url.empty()) {
        SKIP("set MIROTHINKER_API_KEY and MCP_PUBLIC_URL to run the live test");
    }

    MiroThinkerOptions opt;
    opt.api_key = std::move(api_key);
    opt.mcp_url = std::move(mcp_url);
    MiroThinkerClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke(
        {{"user", "Reply with the single word: pong."}},
        "You are a test responder. Keep replies very short.",
        collect_into(events)
    );

    REQUIRE(ok);
    REQUIRE_FALSE(events.empty());

    // CostStatistics is always the terminal event on success.
    REQUIRE(events.back().type == EventType::CostStatistics);
    REQUIRE(events.back().cost.total_tokens > 0);

    // At least one content-bearing event (Reply or Thinking) should land.
    bool saw_content = false;
    for (const auto& e : events) {
        if (e.type == EventType::Reply || e.type == EventType::Thinking) {
            saw_content = true; break;
        }
    }
    REQUIRE(saw_content);

    // No Error events on a healthy round trip.
    for (const auto& e : events) {
        REQUIRE(e.type != EventType::Error);
    }
}

//------------------------------------------------------------------------------

TEST_CASE("live: handler returning false aborts the stream", "[miro-thinker-live][.]") {
    std::string api_key = env_or_empty("MIROTHINKER_API_KEY");
    std::string mcp_url = env_or_empty("MCP_PUBLIC_URL");
    if (api_key.empty() || mcp_url.empty()) {
        SKIP("set MIROTHINKER_API_KEY and MCP_PUBLIC_URL to run the live test");
    }

    MiroThinkerOptions opt;
    opt.api_key = std::move(api_key);
    opt.mcp_url = std::move(mcp_url);
    MiroThinkerClient c{std::move(opt)};

    int seen = 0;
    const bool ok = c.ainvoke(
        {{"user", "Count from 1 to 100, one number per line."}},
        "",
        [&](const Event& /*e*/) {
            ++seen;
            return seen < 2;  // abort after the first delivered event
        }
    );

    REQUIRE_FALSE(ok);
    REQUIRE(seen >= 1);
}
