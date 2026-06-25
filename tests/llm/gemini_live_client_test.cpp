#include "llm/gemini_live.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using mirobody::llm::Event;
using mirobody::llm::EventType;
using mirobody::llm::GeminiLiveClient;
using mirobody::llm::GeminiLiveMode;
using mirobody::llm::GeminiLiveOptions;

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

TEST_CASE("gemini-live ai-studio: missing api_key short-circuits", "[gemini-live]") {
    GeminiLiveOptions opt;
    opt.mode = GeminiLiveMode::AiStudio;
    GeminiLiveClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
    REQUIRE(events[0].content.find("GOOGLE_API_KEY") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("gemini-live vertex: missing access_token short-circuits", "[gemini-live]") {
    GeminiLiveOptions opt;
    opt.mode         = GeminiLiveMode::Vertex;
    opt.gcp_project  = "my-proj";
    opt.gcp_location = "us-central1";
    GeminiLiveClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
    REQUIRE(events[0].content.find("access_token") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("gemini-live vertex: missing project/location short-circuits", "[gemini-live]") {
    GeminiLiveOptions opt;
    opt.mode         = GeminiLiveMode::Vertex;
    opt.access_token = "ya29.fake";
    opt.gcp_project  = "";   // missing
    GeminiLiveClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
    REQUIRE(events[0].content.find("gcp_project") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("gemini-live: empty messages short-circuits", "[gemini-live]") {
    GeminiLiveOptions opt;
    opt.api_key = "fake";
    GeminiLiveClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
}

//------------------------------------------------------------------------------
// Live tests, hidden behind [.].
//   mirobody_tests "[gemini-live-ai-studio-live]"   (needs GOOGLE_API_KEY)
//   mirobody_tests "[gemini-live-vertex-live]"      (needs GCP_ACCESS_TOKEN + GOOGLE_CLOUD_PROJECT)

TEST_CASE("gemini-live ai-studio live: simple round trip", "[gemini-live-ai-studio-live][.]") {
    std::string api_key = env_or_empty("GOOGLE_API_KEY");
    if (api_key.empty()) SKIP("set GOOGLE_API_KEY to run the live test");

    GeminiLiveOptions opt;
    opt.mode    = GeminiLiveMode::AiStudio;
    opt.api_key = std::move(api_key);
    { std::string m = env_or_empty("GEMINI_LIVE_TEST_MODEL"); if (!m.empty()) opt.model = std::move(m); }
    GeminiLiveClient c{std::move(opt)};

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

TEST_CASE("gemini-live vertex live: simple round trip", "[gemini-live-vertex-live][.]") {
    std::string token   = env_or_empty("GCP_ACCESS_TOKEN");
    std::string project = env_or_empty("GOOGLE_CLOUD_PROJECT");
    if (token.empty() || project.empty()) {
        SKIP("set GCP_ACCESS_TOKEN and GOOGLE_CLOUD_PROJECT to run the live test");
    }

    GeminiLiveOptions opt;
    opt.mode         = GeminiLiveMode::Vertex;
    opt.access_token = std::move(token);
    opt.gcp_project  = std::move(project);
    { std::string l = env_or_empty("GOOGLE_CLOUD_LOCATION"); if (!l.empty()) opt.gcp_location = std::move(l); }
    { std::string m = env_or_empty("GEMINI_LIVE_TEST_MODEL"); if (!m.empty()) opt.model = std::move(m); }
    GeminiLiveClient c{std::move(opt)};

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
