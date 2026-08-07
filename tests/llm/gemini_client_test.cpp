#include "client/http_client.hpp"
#include "llm/gemini.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using mirobody::llm::ChatMessage;
using mirobody::llm::Event;
using mirobody::llm::EventType;
using mirobody::llm::GeminiClient;
using mirobody::llm::GeminiMode;
using mirobody::llm::GeminiOptions;

namespace {

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

TEST_CASE("gemini ai-studio: missing api_key short-circuits", "[gemini]") {
    GeminiOptions opt;
    opt.mode = GeminiMode::AiStudio;
    GeminiClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
    REQUIRE(events[0].content.find("GOOGLE_API_KEY") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("gemini vertex: missing access_token short-circuits", "[gemini]") {
    GeminiOptions opt;
    opt.mode         = GeminiMode::Vertex;
    opt.gcp_project  = "my-proj";
    opt.gcp_location = "us-central1";
    GeminiClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
    // What matters is WHICH thing the error blames -- the token, not the project
    // (the next case pins the other side). Matching the whole phrase would pin
    // the wording instead, and the wording is meant to change as the ways of
    // supplying a token do.
    REQUIRE(events[0].content.find("token") != std::string::npos);
}

//------------------------------------------------------------------------------

TEST_CASE("gemini vertex: missing project/location short-circuits", "[gemini]") {
    GeminiOptions opt;
    opt.mode         = GeminiMode::Vertex;
    opt.access_token = "ya29.fake";
    opt.gcp_project  = "";   // missing
    GeminiClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
    REQUIRE(events[0].content.find("gcp_project") != std::string::npos);
}

//------------------------------------------------------------------------------

// The rotation seam: a deployment refreshes the Vertex token under a running
// process, so the token must be resolved at the point of use, not captured when
// the client was built. These pin that the provider is what is consulted -- with
// no `access_token` set at all, only the provider can satisfy (or fail) the
// check, and each turn must ask it again.

TEST_CASE("gemini vertex: the token provider supplies the credential", "[gemini]") {
    GeminiOptions opt;
    opt.mode         = GeminiMode::Vertex;
    opt.gcp_project  = "";   // missing: fails AFTER the token check
    opt.gcp_location = "us-central1";
    opt.access_token = "";   // nothing captured -- the provider is the only source
    opt.access_token_provider = []() { return std::string{"ya29.rotated"}; };
    GeminiClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    // Got past the token check on the provider's value alone: the complaint is
    // about the project, not the credential.
    REQUIRE(events[0].content.find("gcp_project") != std::string::npos);
    REQUIRE(events[0].content.find("access_token") == std::string::npos);
}

TEST_CASE("gemini vertex: an empty provider result reads as a missing token", "[gemini]") {
    GeminiOptions opt;
    opt.mode         = GeminiMode::Vertex;
    opt.gcp_project  = "my-proj";
    opt.gcp_location = "us-central1";
    opt.access_token_provider = []() { return std::string{}; };   // e.g. the token file is not there yet
    GeminiClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
    REQUIRE(events[0].content.find("token") != std::string::npos);
}

TEST_CASE("gemini vertex: the token is re-read for every turn", "[gemini]") {
    // The property that makes rotation work: one client, asked again each turn.
    int calls = 0;
    GeminiOptions opt;
    opt.mode         = GeminiMode::Vertex;
    opt.gcp_project  = "";   // short-circuit after the token check, so no network
    opt.gcp_location = "us-central1";
    opt.access_token_provider = [&calls]() { calls ++; return std::string{"ya29.fake"}; };
    GeminiClient c{std::move(opt)};

    std::vector<Event> events;
    for (int i = 0; i < 3; i ++) { c.ainvoke({{"user", "hi"}}, "", collect_into(events)); }

    REQUIRE(calls == 3);
}

//------------------------------------------------------------------------------

TEST_CASE("gemini: empty messages short-circuits", "[gemini]") {
    GeminiOptions opt;
    opt.api_key = "fake";
    GeminiClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
}

//------------------------------------------------------------------------------
// Live tests, hidden behind [.].
//   mirobody_tests "[gemini-ai-studio-live]"   (needs GOOGLE_API_KEY)
//   mirobody_tests "[gemini-vertex-live]"      (needs GCP_ACCESS_TOKEN + GOOGLE_CLOUD_PROJECT)

TEST_CASE("gemini ai-studio live: simple round trip", "[gemini-ai-studio-live][.]") {
    std::string api_key = env_or_empty("GOOGLE_API_KEY");
    if (api_key.empty()) SKIP("set GOOGLE_API_KEY to run the live test");

    GeminiOptions opt;
    opt.mode    = GeminiMode::AiStudio;
    opt.api_key = std::move(api_key);
    { std::string m = env_or_empty("GEMINI_TEST_MODEL"); if (!m.empty()) opt.model = std::move(m); }
    GeminiClient c{std::move(opt)};

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

TEST_CASE("gemini vertex live: simple round trip", "[gemini-vertex-live][.]") {
    std::string token   = env_or_empty("GCP_ACCESS_TOKEN");
    std::string project = env_or_empty("GOOGLE_CLOUD_PROJECT");
    if (token.empty() || project.empty()) {
        SKIP("set GCP_ACCESS_TOKEN and GOOGLE_CLOUD_PROJECT to run the live test");
    }

    GeminiOptions opt;
    opt.mode         = GeminiMode::Vertex;
    opt.access_token = std::move(token);
    opt.gcp_project  = std::move(project);
    { std::string l = env_or_empty("GOOGLE_CLOUD_LOCATION"); if (!l.empty()) opt.gcp_location = std::move(l); }
    { std::string m = env_or_empty("GEMINI_TEST_MODEL"); if (!m.empty()) opt.model = std::move(m); }
    GeminiClient c{std::move(opt)};

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
