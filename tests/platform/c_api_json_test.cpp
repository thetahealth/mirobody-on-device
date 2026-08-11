#include "platform/c_api_json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

// The C ABI's pure transforms (src/platform/c_api_json.hpp). c_api.cpp itself is
// untestable here -- every entry point opens with the process-wide config, which
// comes from a remote store -- so these functions ARE the coverage for the FFI
// surface's real logic. They are also the only description of that logic: the C
// contract in src/mirobody.h documents the JSON shapes, and what follows is what
// the implementation actually does with a payload that does not match.

namespace pf = mirobody::platform;

//------------------------------------------------------------------------------
// split_provider
//------------------------------------------------------------------------------

TEST_CASE("split_provider splits an Agent/model pair", "[c_api]") {
    const pf::ProviderToken t = pf::split_provider("baseline/gpt-4o");
    REQUIRE(t.agent == "baseline");
    REQUIRE(t.model == "gpt-4o");
}

TEST_CASE("split_provider keeps a slash-less token whole", "[c_api]") {
    // No '/' => the token might be an agent name OR a bare model from the
    // prefix-less list; c_api hands the whole thing to resolve_agent, so it must
    // arrive intact in `agent` rather than being guessed at here.
    const pf::ProviderToken t = pf::split_provider("mirothinker");
    REQUIRE(t.agent == "mirothinker");
    REQUIRE(t.model.empty());
}

TEST_CASE("split_provider handles empty and edge tokens", "[c_api]") {
    SECTION("empty token defaults everything") {
        const pf::ProviderToken t = pf::split_provider("");
        REQUIRE(t.agent.empty());
        REQUIRE(t.model.empty());
    }
    SECTION("trailing slash means an empty model, not a missing one") {
        const pf::ProviderToken t = pf::split_provider("baseline/");
        REQUIRE(t.agent == "baseline");
        REQUIRE(t.model.empty());
    }
    SECTION("only the FIRST slash splits -- model names may contain more") {
        const pf::ProviderToken t = pf::split_provider("baseline/openai/gpt-4o");
        REQUIRE(t.agent == "baseline");
        REQUIRE(t.model == "openai/gpt-4o");
    }
}

//------------------------------------------------------------------------------
// parse_chat_messages
//------------------------------------------------------------------------------

TEST_CASE("parse_chat_messages reads a conversation in order", "[c_api]") {
    const pf::ChatInput in = pf::parse_chat_messages(
        "[{\"role\":\"system\",\"content\":\"be brief\"},"
        " {\"role\":\"user\",\"content\":\"hi\"},"
        " {\"role\":\"assistant\",\"content\":\"hello\"}]");

    REQUIRE(in.error == pf::ChatInputError::None);
    REQUIRE(in.messages.size() == 3);
    REQUIRE(in.messages[0].role == "system");
    REQUIRE(in.messages[1].content == "hi");
    REQUIRE(in.messages[2].role == "assistant");
}

TEST_CASE("parse_chat_messages takes the LAST user turn as the question", "[c_api]") {
    // `question` becomes persist_history's summary, so it must be the turn being
    // asked now -- not the first one in the transcript.
    const pf::ChatInput in = pf::parse_chat_messages(
        "[{\"role\":\"user\",\"content\":\"first\"},"
        " {\"role\":\"assistant\",\"content\":\"ok\"},"
        " {\"role\":\"user\",\"content\":\"second\"}]");

    REQUIRE(in.error == pf::ChatInputError::None);
    REQUIRE(in.question == "second");
}

TEST_CASE("parse_chat_messages leaves the question empty with no user turn", "[c_api]") {
    const pf::ChatInput in = pf::parse_chat_messages(
        "[{\"role\":\"system\",\"content\":\"be brief\"}]");

    REQUIRE(in.error == pf::ChatInputError::None);
    REQUIRE(in.messages.size() == 1);
    REQUIRE(in.question.empty());
}

TEST_CASE("parse_chat_messages skips unusable entries rather than failing", "[c_api]") {
    // Tolerance is the contract: one malformed entry from a host must not sink a
    // turn that has perfectly good messages around it.
    const pf::ChatInput in = pf::parse_chat_messages(
        "[\"not an object\","
        " {\"role\":\"user\"},"                              // no content
        " {\"content\":\"orphan\"},"                         // no role
        " {\"role\":42,\"content\":\"wrong type\"},"         // role not a string
        " {\"role\":\"user\",\"content\":123},"              // content not a string
        " {\"role\":\"user\",\"content\":\"keep me\"}]");

    REQUIRE(in.error == pf::ChatInputError::None);
    REQUIRE(in.messages.size() == 1);
    REQUIRE(in.messages[0].content == "keep me");
    REQUIRE(in.question == "keep me");
}

TEST_CASE("parse_chat_messages reports a malformed payload apart from an empty one", "[c_api]") {
    // c_api logs these two differently (and a host debugging its own JSON needs
    // that), so the distinction has to survive in the return value.
    SECTION("null pointer") {
        REQUIRE(pf::parse_chat_messages(nullptr).error == pf::ChatInputError::NotAnArray);
    }
    SECTION("unparseable") {
        REQUIRE(pf::parse_chat_messages("[{").error == pf::ChatInputError::NotAnArray);
    }
    SECTION("valid JSON but not an array") {
        REQUIRE(pf::parse_chat_messages("{\"role\":\"user\",\"content\":\"hi\"}").error ==
                pf::ChatInputError::NotAnArray);
    }
    SECTION("empty array") {
        REQUIRE(pf::parse_chat_messages("[]").error == pf::ChatInputError::NoUsableMessages);
    }
    SECTION("array of nothing usable") {
        REQUIRE(pf::parse_chat_messages("[{\"role\":\"user\"}]").error ==
                pf::ChatInputError::NoUsableMessages);
    }
}

//------------------------------------------------------------------------------
// flatten_observation
//------------------------------------------------------------------------------

namespace {

pf::HealthItem flatten(const std::string& json) {
    pf::HealthItem item;
    REQUIRE(pf::flatten_observation_json(json.c_str(), json.size(), item));
    return item;
}

}   // namespace

TEST_CASE("flatten_observation reads a plain instant sample", "[c_api]") {
    const pf::HealthItem it = flatten(
        "{\"resourceType\":\"Observation\","
        " \"code\":{\"coding\":[{\"code\":\"8867-4\",\"display\":\"Heart rate\"}]},"
        " \"valueQuantity\":{\"value\":72,\"unit\":\"beats/minute\",\"code\":\"/min\"},"
        " \"effectiveDateTime\":\"2026-08-07T09:15:00Z\","
        " \"method\":{\"coding\":[{\"display\":\"Apple Watch\"}]}}");

    REQUIRE(it.code == "8867-4");
    REQUIRE(it.display == "Heart rate");
    REQUIRE(it.has_value);
    REQUIRE(it.value == 72.0);
    REQUIRE(it.unit == "/min");            // UCUM code wins over the human label
    REQUIRE(it.when == "2026-08-07T09:15:00Z");
    REQUIRE(it.source == "Apple Watch");
}

TEST_CASE("flatten_observation falls back to the human unit with no UCUM code", "[c_api]") {
    const pf::HealthItem it = flatten(
        "{\"valueQuantity\":{\"value\":8000,\"unit\":\"steps\"}}");

    REQUIRE(it.has_value);
    REQUIRE(it.unit == "steps");
}

TEST_CASE("flatten_observation takes a panel's value from component[0]", "[c_api]") {
    // Blood pressure: the Observation itself carries no valueQuantity, only
    // components. Without this fallback every BP reading lists as value-less.
    const pf::HealthItem it = flatten(
        "{\"code\":{\"coding\":[{\"code\":\"85354-9\",\"display\":\"Blood pressure\"}]},"
        " \"component\":["
        "   {\"code\":{\"coding\":[{\"code\":\"8480-6\"}]},"
        "    \"valueQuantity\":{\"value\":118,\"code\":\"mm[Hg]\"}},"
        "   {\"code\":{\"coding\":[{\"code\":\"8462-4\"}]},"
        "    \"valueQuantity\":{\"value\":76,\"code\":\"mm[Hg]\"}}]}");

    REQUIRE(it.code == "85354-9");         // the panel's own code, not the component's
    REQUIRE(it.has_value);
    REQUIRE(it.value == 118.0);            // systolic -- the FIRST component
    REQUIRE(it.unit == "mm[Hg]");
}

TEST_CASE("flatten_observation prefers a top-level value over a component", "[c_api]") {
    const pf::HealthItem it = flatten(
        "{\"valueQuantity\":{\"value\":1,\"code\":\"top\"},"
        " \"component\":[{\"valueQuantity\":{\"value\":2,\"code\":\"comp\"}}]}");

    REQUIRE(it.value == 1.0);
    REQUIRE(it.unit == "top");
}

TEST_CASE("flatten_observation falls through a malformed valueQuantity to the component",
          "[c_api]") {
    // A non-object valueQuantity is not a value; the panel path is still open.
    // (This is the case the old iterator juggling in c_api.cpp handled by
    // rewriting an iterator from one object into another's end sentinel.)
    const pf::HealthItem it = flatten(
        "{\"valueQuantity\":\"nonsense\","
        " \"component\":[{\"valueQuantity\":{\"value\":5,\"code\":\"mg\"}}]}");

    REQUIRE(it.has_value);
    REQUIRE(it.value == 5.0);
    REQUIRE(it.unit == "mg");
}

TEST_CASE("flatten_observation reports no value when neither path yields one", "[c_api]") {
    SECTION("malformed valueQuantity, no component") {
        const pf::HealthItem it = flatten("{\"valueQuantity\":\"nonsense\"}");
        REQUIRE_FALSE(it.has_value);
        REQUIRE(it.value == 0.0);
        REQUIRE(it.unit.empty());
    }
    SECTION("component without a valueQuantity") {
        const pf::HealthItem it = flatten("{\"component\":[{\"code\":{}}]}");
        REQUIRE_FALSE(it.has_value);
    }
    SECTION("empty component array") {
        const pf::HealthItem it = flatten("{\"component\":[]}");
        REQUIRE_FALSE(it.has_value);
    }
    SECTION("a non-numeric value still yields the unit") {
        const pf::HealthItem it = flatten("{\"valueQuantity\":{\"value\":\"72\",\"code\":\"/min\"}}");
        REQUIRE_FALSE(it.has_value);
        REQUIRE(it.unit == "/min");
    }
}

TEST_CASE("flatten_observation falls back to effectivePeriod.start", "[c_api]") {
    // Interval samples -- a day's steps, a night's sleep -- carry no
    // effectiveDateTime. Their start time is what orders them against everything
    // else, so losing it would drop them to the bottom of every listing.
    const pf::HealthItem it = flatten(
        "{\"effectivePeriod\":{\"start\":\"2026-08-06T00:00:00Z\","
        " \"end\":\"2026-08-07T00:00:00Z\"}}");

    REQUIRE(it.when == "2026-08-06T00:00:00Z");
}

TEST_CASE("flatten_observation prefers effectiveDateTime over the period", "[c_api]") {
    const pf::HealthItem it = flatten(
        "{\"effectiveDateTime\":\"2026-08-07T09:00:00Z\","
        " \"effectivePeriod\":{\"start\":\"2026-08-06T00:00:00Z\"}}");

    REQUIRE(it.when == "2026-08-07T09:00:00Z");
}

TEST_CASE("flatten_observation treats every field as optional", "[c_api]") {
    // A bare object is still a row worth listing; the C contract lets each field
    // come back empty rather than the whole listing fail.
    SECTION("empty object") {
        const pf::HealthItem it = flatten("{}");
        REQUIRE(it.code.empty());
        REQUIRE(it.display.empty());
        REQUIRE(it.when.empty());
        REQUIRE(it.source.empty());
        REQUIRE_FALSE(it.has_value);
    }
    SECTION("wrong types throughout") {
        const pf::HealthItem it = flatten(
            "{\"code\":\"str\",\"method\":[],\"effectivePeriod\":7,\"component\":{}}");
        REQUIRE(it.code.empty());
        REQUIRE(it.source.empty());
        REQUIRE(it.when.empty());
    }
    SECTION("an empty coding array is not a coding") {
        const pf::HealthItem it = flatten("{\"code\":{\"coding\":[]}}");
        REQUIRE(it.code.empty());
    }
}

TEST_CASE("flatten_observation_json rejects text that is not a JSON object", "[c_api]") {
    // The caller skips the row on false; anything else would fail a whole page
    // of readings over one bad row.
    pf::HealthItem it;
    SECTION("null")        { REQUIRE_FALSE(pf::flatten_observation_json(nullptr, 0, it)); }
    SECTION("empty")       { REQUIRE_FALSE(pf::flatten_observation_json("", 0, it)); }
    SECTION("truncated")   { REQUIRE_FALSE(pf::flatten_observation_json("{\"code\":", 8, it)); }
    SECTION("an array")    { REQUIRE_FALSE(pf::flatten_observation_json("[]", 2, it)); }
    SECTION("a bare scalar") { REQUIRE_FALSE(pf::flatten_observation_json("42", 2, it)); }
}

//------------------------------------------------------------------------------
// later_reading
//------------------------------------------------------------------------------

TEST_CASE("later_reading orders newest reading first", "[c_api]") {
    // The store pages by write time, which within one sync is a single instant --
    // so this comparator, not the query, is what puts a batch in time order.
    std::vector<pf::HealthItem> items(4);
    items[0].when = "2026-08-05T00:00:00Z";
    items[1].when = "";                        // no effective time
    items[2].when = "2026-08-07T00:00:00Z";
    items[3].when = "2026-08-06T00:00:00Z";

    std::sort(items.begin(), items.end(), pf::later_reading);

    REQUIRE(items[0].when == "2026-08-07T00:00:00Z");
    REQUIRE(items[1].when == "2026-08-06T00:00:00Z");
    REQUIRE(items[2].when == "2026-08-05T00:00:00Z");
    REQUIRE(items[3].when.empty());            // time-less items sort LAST, not first
}

TEST_CASE("later_reading is a strict weak ordering", "[c_api]") {
    // std::sort is undefined behaviour on a comparator that says a < a, and two
    // readings sharing an instant is routine in a sync batch.
    pf::HealthItem a, b, empty1, empty2;
    a.when = b.when = "2026-08-07T00:00:00Z";

    REQUIRE_FALSE(pf::later_reading(a, a));
    REQUIRE_FALSE(pf::later_reading(a, b));
    REQUIRE_FALSE(pf::later_reading(b, a));
    REQUIRE_FALSE(pf::later_reading(empty1, empty2));
    REQUIRE_FALSE(pf::later_reading(empty2, empty1));
}
