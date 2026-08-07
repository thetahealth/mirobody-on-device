#include "chat/agent.hpp"

#include "llm/client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using mirobody::chat::Agent;
using mirobody::chat::AgentRegistration;
using mirobody::chat::AgentRegistry;
using mirobody::chat::AgentRequest;
using mirobody::chat::ClientMap;
using mirobody::chat::agent_registry;
using mirobody::chat::detect_language;
using mirobody::llm::ChatMessage;
using mirobody::llm::Client;
using mirobody::llm::Event;
using mirobody::llm::EventHandler;
using mirobody::llm::EventType;
using mirobody::llm::UserContext;

namespace {

// A canned client that emits one Reply event echoing the system prompt, so a
// test can assert the agent forwarded the right prompt without any network.
class StubClient : public Client {
public:
    bool ainvoke(const std::vector<ChatMessage>& messages,
                 const std::string&              system_prompt,
                 const EventHandler&             on_event,
                 const UserContext&              /*user*/ = UserContext()) override {
        Event e;
        e.type = EventType::Reply;
        e.content = "prompt=" + system_prompt + " turns=" + std::to_string(messages.size());
        on_event(e);
        return true;
    }
};

// A trivial agent that streams whatever client it was given for "p".
class StubAgent : public Agent {
public:
    void generate_response(const AgentRequest& req, const EventHandler& on_event) override {
        std::shared_ptr<Client> c = agent_registry().client("Stub", req.provider);
        if (!c) {
            Event e; e.type = EventType::Error; e.content = "no client";
            on_event(e);
            return;
        }
        c->ainvoke(req.messages, "sys", on_event);
    }
};

}   // namespace

//------------------------------------------------------------------------------
// detect_language -- the Unicode-script heuristic ported from chat/agent.py.
//------------------------------------------------------------------------------

TEST_CASE("detect_language recognizes scripts", "[agent]") {
    REQUIRE(detect_language("\xE3\x81\x93\xE3\x82\x93")           == "Japanese");        // こん
    REQUIRE(detect_language("\xEC\x95\x88\xEB\x85\x95")           == "Korean");          // 안녕
    REQUIRE(detect_language("\xE4\xBD\xA0\xE5\xA5\xBD")           == "Simplified Chinese"); // 你好
    REQUIRE(detect_language("\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2")   == "Russian");         // прив
    REQUIRE(detect_language("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D")   == "Hebrew");          // שלום
    REQUIRE(detect_language("\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7") == "Arabic");    // مرحبا
}

TEST_CASE("detect_language falls through for Latin / empty", "[agent]") {
    // Empty input with no Accept-Language means no language requirement: the
    // caller drops the instruction from the system prompt.
    REQUIRE(detect_language("").empty());
    // Latin script returns the "same language as the user" instruction.
    REQUIRE(detect_language("hello").find("hello") != std::string::npos);
}

TEST_CASE("detect_language uses Accept-Language as a fallback", "[agent]") {
    // A recognized script always wins over the header.
    REQUIRE(detect_language("\xE4\xBD\xA0\xE5\xA5\xBD", "fr-FR") == "Simplified Chinese");

    // Empty (upload-only) turn adopts the client's preferred language.
    REQUIRE(detect_language("", "fr-FR")  == "French");
    REQUIRE(detect_language("", "ja")     == "Japanese");
    REQUIRE(detect_language("", "zh-TW")  == "Traditional Chinese");
    REQUIRE(detect_language("", "zh-CN")  == "Simplified Chinese");
    // An unrecognized tag is passed through by BCP-47 code.
    REQUIRE(detect_language("", "sw-KE").find("sw-KE") != std::string::npos);

    // Latin-script message still mirrors the user, but names the preferred
    // language as the disambiguation fallback.
    const std::string latin = detect_language("ok", "de-DE");
    REQUIRE(latin.find("ok") != std::string::npos);
    REQUIRE(latin.find("German") != std::string::npos);
}

//------------------------------------------------------------------------------
// Registry behavior, exercised on a local instance (no global state).
//------------------------------------------------------------------------------

TEST_CASE("registry add / find / names / visibility", "[agent]") {
    AgentRegistry reg;

    AgentRegistration pub;
    pub.name = "Alpha"; pub.is_public = true;
    pub.factory = [](const AgentRequest&) { return std::unique_ptr<Agent>(new StubAgent()); };
    REQUIRE(reg.add(pub));

    AgentRegistration priv;
    priv.name = "Beta"; priv.is_public = false;
    priv.factory = [](const AgentRequest&) { return std::unique_ptr<Agent>(new StubAgent()); };
    REQUIRE(reg.add(priv));

    REQUIRE(reg.size() == 2);
    REQUIRE(reg.find("Alpha") != nullptr);
    REQUIRE(reg.find("nope")  == nullptr);
    REQUIRE(reg.names().size() == 2);
    REQUIRE(reg.names(/*public_only=*/true).size() == 1);
    REQUIRE(reg.names(true)[0] == "Alpha");

    // Duplicate name is ignored.
    REQUIRE(reg.add(pub));
    REQUIRE(reg.size() == 2);
}

TEST_CASE("registry create instantiates via the factory", "[agent]") {
    AgentRegistry reg;
    AgentRegistration r;
    r.name = "Alpha"; r.is_public = true;
    r.factory = [](const AgentRequest&) { return std::unique_ptr<Agent>(new StubAgent()); };
    reg.add(r);

    AgentRequest req;
    REQUIRE(reg.create("Alpha", req) != nullptr);
    REQUIRE(reg.create("missing", req) == nullptr);
}

//------------------------------------------------------------------------------
// The full loop on the global registry: a stub agent + stub client wired via
// load_clients, then create() + generate_response() streaming an event.
//------------------------------------------------------------------------------

TEST_CASE("global registry: load_clients -> create -> stream", "[agent]") {
    AgentRegistration r;
    r.name = "Stub"; r.is_public = false;
    r.factory = [](const AgentRequest&) { return std::unique_ptr<Agent>(new StubAgent()); };
    r.load_clients = [](const mirobody::Config&) {
        ClientMap m;
        m["p"] = std::make_shared<StubClient>();
        return m;
    };
    agent_registry().add(r);

    mirobody::Config cfg;
    agent_registry().load_clients(cfg);

    REQUIRE(agent_registry().client("Stub", "p") != nullptr);
    REQUIRE(agent_registry().client("Stub", "absent") == nullptr);

    AgentRequest req;
    req.provider = "p";
    req.messages.push_back({"user", "hi"});

    std::unique_ptr<Agent> a = agent_registry().create("Stub", req);
    REQUIRE(a != nullptr);

    std::string seen;
    a->generate_response(req, [&](const Event& e) {
        if (e.type == EventType::Reply) seen = e.content;
        return true;
    });
    REQUIRE(seen == "prompt=sys turns=1");
}

//------------------------------------------------------------------------------
// BaselineAgent self-registered from res/agents/baseline.cpp (linked via
// $<TARGET_OBJECTS:agent_impls>). Mirrors the MCP "tools self-register" guard.
//------------------------------------------------------------------------------

TEST_CASE("BaselineAgent self-registers", "[agent]") {
    const AgentRegistration* base = agent_registry().find("Baseline");
    REQUIRE(base != nullptr);
    REQUIRE(base->is_public);
    REQUIRE(base->factory);
}

TEST_CASE("a provider is offered exactly when it has a credential", "[agent]") {
    // The rule from res/agents/baseline.cpp offer(): an unconfigured provider is
    // never listed, because it cannot answer a single turn and would be a dead row
    // in the model picker that errors the moment it is used.
    //
    // Asserted as an EQUIVALENCE against the credential each provider actually
    // resolves, rather than by pinning the credentials first. That matters because
    // ConfigStore::get_str reads the ENVIRONMENT before its YAML and an empty YAML
    // scalar cannot override it — so a machine exporting GOOGLE_API_KEY genuinely
    // does have that credential. Testing "offered == has credential" holds on such a
    // machine and on a bare CI box alike; asserting "empty config => nothing offered"
    // would pass on one and fail on the other.
    mirobody::Config cfg;
    agent_registry().load_clients(cfg);

    struct Expect {
        const char* model;
        std::string credential;   // resolved the same way baseline.cpp resolves it
    };
    std::vector<Expect> expected;
    expected.push_back({"gpt-5-nano",      cfg.openai.api_key});
    expected.push_back({"mirothinker-1.7", cfg.store.get_str("MIROTHINKER_API_KEY")});

    // The Gemini rows depend on the surface, so the expectation has to as well --
    // and for the same reason the comment above gives, the surface is read from
    // the environment here rather than pinned: a machine exporting
    // GOOGLE_CLOUD_PROJECT + GOOGLE_CLOUD_LOCATION genuinely IS a Vertex
    // deployment, and would be offered the Vertex model list. Pinning either
    // side would pass on one machine and fail on the other.
    const std::string project  = cfg.store.get_str("GOOGLE_CLOUD_PROJECT");
    const std::string location = cfg.store.get_str("GOOGLE_CLOUD_LOCATION");
    if (!project.empty() && !location.empty()) {
        // Vertex: the project stands in for the credential (the token is
        // resolved per request and may be ambient), and 3.5 replaces 3.6, which
        // publishes only the `global` location.
        expected.push_back({"gemini-2.5-flash", project});
        expected.push_back({"gemini-3.5-flash", project});
    } else {
        // AI Studio: both rows ride the one key on desktop; mobile lists only 3.6.
        const std::string key = cfg.store.get_str("GOOGLE_API_KEY", cfg.gemini.api_key);
        expected.push_back({"gemini-2.5-flash", key});
        expected.push_back({"gemini-3.6-flash", key});
    }

    const std::vector<std::string> names = agent_registry().provider_names(true);
    for (const Expect& e : expected) {
        const bool has_credential = !e.credential.empty();
        CAPTURE(e.model, has_credential);
        CHECK((agent_registry().client("Baseline", e.model) != nullptr) == has_credential);
        CHECK((std::find(names.begin(), names.end(), e.model) != names.end()) == has_credential);
    }
}

TEST_CASE("BaselineAgent loads its three providers once they have credentials", "[agent]") {
    mirobody::Config cfg;
    // One credential per provider — whatever makes it callable. The values are
    // nonsense on purpose: offer() only checks presence, and no request is made.
    // gemini and mirothinker read theirs through ConfigStore, where the environment
    // outranks YAML — so this works either way: on CI the YAML supplies them, on a
    // machine that exports the real keys the environment does.
    cfg.openai.api_key = "sk-test";
    REQUIRE(cfg.store.load_yaml_string(
        "GOOGLE_API_KEY: gm-test\n"
        "MIROTHINKER_API_KEY: mt-test\n"
        "MCP_PUBLIC_URL: http://127.0.0.1/mcp\n"));

    agent_registry().load_clients(cfg);

    REQUIRE(agent_registry().client("Baseline", "gpt-5-nano")       != nullptr);
    REQUIRE(agent_registry().client("Baseline", "gemini-2.5-flash") != nullptr);
    REQUIRE(agent_registry().client("Baseline", "mirothinker-1.7")  != nullptr);

    // Baseline is the default agent, so its providers surface in the discovery
    // list WITHOUT the "Baseline/" prefix -- just the model name.
    const std::vector<std::string> names = agent_registry().provider_names(true);
    REQUIRE(std::find(names.begin(), names.end(), "gpt-5-nano")       != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "gemini-2.5-flash") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "mirothinker-1.7")  != names.end());
    // The old prefixed form no longer leaks in.
    REQUIRE(std::find(names.begin(), names.end(), "Baseline/gpt-5-nano") == names.end());
}

TEST_CASE("resolve_agent maps bare providers and empty tokens to the default agent", "[agent]") {
    mirobody::Config cfg;
    agent_registry().load_clients(cfg);

    // A bare provider (the prefix-less list) routes through the default agent,
    // carried as the provider.
    std::string prov;
    REQUIRE(agent_registry().resolve_agent("gemini-2.5-flash", prov) == "Baseline");
    REQUIRE(prov == "gemini-2.5-flash");

    // An empty token defaults to the agent, leaving an explicit provider intact.
    std::string prov2 = "gpt-5-nano";
    REQUIRE(agent_registry().resolve_agent("", prov2) == "Baseline");
    REQUIRE(prov2 == "gpt-5-nano");

    // A real agent name passes through unchanged.
    std::string prov3;
    REQUIRE(agent_registry().resolve_agent("Baseline", prov3) == "Baseline");
    REQUIRE(prov3.empty());
}
