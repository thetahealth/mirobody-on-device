#include "llm/openai_realtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using mirobody::llm::Event;
using mirobody::llm::EventType;
using mirobody::llm::OpenAIRealtimeClient;
using mirobody::llm::OpenAIRealtimeMode;
using mirobody::llm::OpenAIRealtimeOptions;

namespace {

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string{v} : std::string{};
}

auto collect_into(std::vector<Event>& sink) {
    return [&sink](const Event& e) { sink.push_back(e); return true; };
}

}

//------------------------------------------------------------------------------
// Validation: these short-circuit before any socket is opened, so they run with
// no network and no credentials.

TEST_CASE("openai-realtime openai: missing api_key short-circuits", "[openai-realtime]") {
    OpenAIRealtimeOptions opt;
    opt.mode = OpenAIRealtimeMode::OpenAI;
    OpenAIRealtimeClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
    REQUIRE(events[0].content.find("OPENAI_API_KEY") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("openai-realtime azure: missing api_key short-circuits", "[openai-realtime]") {
    OpenAIRealtimeOptions opt;
    opt.mode             = OpenAIRealtimeMode::Azure;
    opt.azure_endpoint   = "wss://my.openai.azure.com";
    opt.azure_deployment = "my-realtime";
    OpenAIRealtimeClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
    REQUIRE(events[0].content.find("AZURE_OPENAI_API_KEY") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("openai-realtime azure: missing endpoint/deployment short-circuits", "[openai-realtime]") {
    OpenAIRealtimeOptions opt;
    opt.mode           = OpenAIRealtimeMode::Azure;
    opt.api_key        = "fake";
    opt.azure_endpoint = "";   // missing
    OpenAIRealtimeClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
    REQUIRE(events[0].content.find("azure_endpoint") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("openai-realtime: empty messages short-circuits", "[openai-realtime]") {
    OpenAIRealtimeOptions opt;
    opt.api_key = "fake";
    OpenAIRealtimeClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
}

//------------------------------------------------------------------------------
// Live tests, hidden behind [.].
//   mirobody_tests "[openai-realtime-live]"        (needs OPENAI_API_KEY)
//   mirobody_tests "[openai-realtime-azure-live]"  (needs AZURE_OPENAI_API_KEY +
//                                                   AZURE_OPENAI_ENDPOINT + AZURE_OPENAI_DEPLOYMENT)

TEST_CASE("openai-realtime live: simple round trip", "[openai-realtime-live][.]") {
    std::string api_key = env_or_empty("OPENAI_API_KEY");
    if (api_key.empty()) SKIP("set OPENAI_API_KEY to run the live test");

    OpenAIRealtimeOptions opt;
    opt.mode    = OpenAIRealtimeMode::OpenAI;
    opt.api_key = std::move(api_key);
    { std::string m = env_or_empty("OPENAI_REALTIME_TEST_MODEL"); if (!m.empty()) opt.model = std::move(m); }
    OpenAIRealtimeClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke(
        {{"user", "Reply with the single word: pong."}},
        "Reply tersely.",
        collect_into(events)
    );

    REQUIRE(ok);
    REQUIRE_FALSE(events.empty());
    REQUIRE(events.back().type == EventType::CostStatistics);
    for (const auto& e : events) REQUIRE(e.type != EventType::Error);
}

//------------------------------------------------------------------------------

TEST_CASE("openai-realtime azure live: simple round trip", "[openai-realtime-azure-live][.]") {
    std::string api_key    = env_or_empty("AZURE_OPENAI_API_KEY");
    std::string endpoint   = env_or_empty("AZURE_OPENAI_ENDPOINT");
    std::string deployment = env_or_empty("AZURE_OPENAI_DEPLOYMENT");
    if (api_key.empty() || endpoint.empty() || deployment.empty()) {
        SKIP("set AZURE_OPENAI_API_KEY, AZURE_OPENAI_ENDPOINT and AZURE_OPENAI_DEPLOYMENT to run the live test");
    }

    OpenAIRealtimeOptions opt;
    opt.mode             = OpenAIRealtimeMode::Azure;
    opt.api_key          = std::move(api_key);
    opt.azure_endpoint   = std::move(endpoint);
    opt.azure_deployment = std::move(deployment);
    { std::string v = env_or_empty("OPENAI_REALTIME_API_VERSION"); if (!v.empty()) opt.api_version = std::move(v); }
    OpenAIRealtimeClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke(
        {{"user", "Reply with the single word: pong."}},
        "Reply tersely.",
        collect_into(events)
    );

    REQUIRE(ok);
    REQUIRE_FALSE(events.empty());
    REQUIRE(events.back().type == EventType::CostStatistics);
    for (const auto& e : events) REQUIRE(e.type != EventType::Error);
}
