#pragma once

// The pure transforms behind the C ABI — the parts of c_api.cpp that are only
// JSON and strings.
//
// They exist as their own translation unit for one reason: testability. Every C
// entry point in c_api.cpp opens with the process-wide config (and from there a
// database, an object store, provider clients), which a unit test cannot stand
// up — the config comes from a remote store, not a file. The logic worth
// covering, though, touches none of that: flattening a FHIR Observation, pulling
// a conversation out of a JSON array, splitting an "Agent/model" token. Lifted
// here, that logic is reachable from tests/platform/c_api_json_test.cpp while
// the C functions stay the thin, config-bound shells they are.
//
// Nothing here reads global state, opens a connection, or logs. Callers decide
// what a failure means: c_api.cpp turns each one into its own log line and
// return code, and a test just asserts on the returned value.

#include "llm/mirothinker.hpp"   // llm::ChatMessage

#include <rapidjson/document.h>

#include <cstddef>
#include <string>
#include <vector>

namespace mirobody { namespace platform {

//------------------------------------------------------------------------------
// Provider tokens
//------------------------------------------------------------------------------

// The two halves of an "Agent/model" pair as mirobody_get_providers reports it.
// With no '/' the whole token lands in `agent` and `model` is empty: it may be a
// bare model name from the prefix-less list, which only the agent registry can
// tell apart, so resolve_agent() makes that call downstream.
struct ProviderToken {
    std::string agent;
    std::string model;
};

ProviderToken split_provider(const std::string& pair);

//------------------------------------------------------------------------------
// Conversation input
//------------------------------------------------------------------------------

// Why a messages_json payload yielded no conversation. The two cases are
// distinct on purpose: a malformed payload is the host's bug, an array that
// held nothing usable is a different one, and mirobody_chat_messages logs them
// apart even though both return -1.
enum class ChatInputError {
    None,
    NotAnArray,          // absent, unparseable, or a non-array document
    NoUsableMessages     // parsed fine, but no entry had both role and content
};

struct ChatInput {
    std::vector<llm::ChatMessage> messages;
    std::string                   question;   // the LAST user turn (persist_history's summary)
    ChatInputError                error = ChatInputError::None;
};

// Extraction is tolerant: an entry that is not an object, or is missing a string
// role or content, is skipped rather than failing the whole turn.
ChatInput parse_chat_messages(const char* messages_json);

//------------------------------------------------------------------------------
// Health readings
//------------------------------------------------------------------------------

// One flattened Observation, as mirobody_health_recent emits it.
struct HealthItem {
    std::string code, display, unit, when, source;
    double      value     = 0.0;
    bool        has_value = false;
};

// Pull the fields mirobody_health_recent reports out of one Observation:
// code.coding[0], valueQuantity (falling back to component[0] for panels such as
// blood pressure), effectiveDateTime (falling back to effectivePeriod.start for
// interval samples such as a day's steps), and method.coding[0].display. Every
// field is optional — an Observation missing all of them flattens to an empty
// item rather than an error.
HealthItem flatten_observation(const rapidjson::Value& resource);

// Same, from the stored JSON text. Returns false when the text is not a readable
// JSON object, which is the caller's cue to skip that row instead of failing the
// whole listing.
bool flatten_observation_json(const char* json, std::size_t len, HealthItem& out);

// Newest READING first, for std::sort. The store pages by updated_at (write
// time), which within one sync is effectively one instant, so its tie-break —
// resource_id — would group a batch by metric name instead of ordering it in
// time. FHIR instants are ISO-8601 UTC, so comparing the strings orders them; an
// item with no effective time sorts last rather than being dropped.
bool later_reading(const HealthItem& a, const HealthItem& b);

}
}
