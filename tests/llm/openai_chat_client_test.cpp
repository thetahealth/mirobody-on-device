#include "client/http_client.hpp"
#include "llm/openai_chat.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using mirobody::llm::ChatMessage;
using mirobody::llm::Event;
using mirobody::llm::EventType;
using mirobody::llm::OpenAIChatClient;
using mirobody::llm::OpenAIChatOptions;

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

TEST_CASE("openai-chat: missing api_key short-circuits", "[openai-chat]") {
    OpenAIChatOptions opt;
    OpenAIChatClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({{"user", "hi"}}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
}

//------------------------------------------------------------------------------

TEST_CASE("openai-chat: empty messages short-circuits", "[openai-chat]") {
    OpenAIChatOptions opt;
    opt.api_key = "sk-fake";
    OpenAIChatClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke({}, "", collect_into(events));

    REQUIRE_FALSE(ok);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].type == EventType::Error);
}

//------------------------------------------------------------------------------
// Live test. Hidden behind [.] — run with:
//   mirobody_tests "[openai-chat-live]"
// after exporting OPENAI_API_KEY (and optionally OPENAI_BASE_URL / model).

TEST_CASE("openai-chat live: round trip yields content + CostStatistics",
          "[openai-chat-live][.]") {
    std::string api_key = env_or_empty("OPENAI_API_KEY");
    if (api_key.empty()) SKIP("set OPENAI_API_KEY to run the live test");

    OpenAIChatOptions opt;
    opt.api_key = std::move(api_key);
    {
        std::string b = env_or_empty("OPENAI_BASE_URL");
        if (!b.empty()) opt.base_url = std::move(b);
    }
    {
        std::string m = env_or_empty("OPENAI_TEST_MODEL");
        if (!m.empty()) opt.model = std::move(m);
    }
    OpenAIChatClient c{std::move(opt)};

    std::vector<Event> events;
    const bool ok = c.ainvoke(
        {{"user", "Reply with the single word: pong."}},
        "You are a test responder. Reply tersely.",
        collect_into(events)
    );

    REQUIRE(ok);
    REQUIRE_FALSE(events.empty());
    REQUIRE(events.back().type == EventType::CostStatistics);
    REQUIRE(events.back().cost.total_tokens > 0);

    bool saw_reply = false;
    for (const auto& e : events) {
        if (e.type == EventType::Reply) { saw_reply = true; break; }
    }
    REQUIRE(saw_reply);
    for (const auto& e : events) REQUIRE(e.type != EventType::Error);
}
