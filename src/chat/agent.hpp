#pragma once

// Agent framework: contract, registry, and per-agent LLM-client store.
//
// The C++ port of the Python reference's chat/agent.py. Python discovered
// agent classes by reflection (any class with a generate_response method, with
// the "Agent" suffix stripped to name it) and held their built LLM clients in
// a global dict. C++ has no standard reflection, so -- exactly as with the MCP tool
// registry -- an agent instead registers a factory under its name, and the
// concrete agents live in res/agents/*.cpp, each self-registering via
// MIROBODY_REGISTER_AGENT. Discovery is compile-time (glob + OBJECT library);
// see CMakeLists.txt.
//
// An agent's streamed output (Python's `AsyncGenerator[dict]`) maps onto the
// existing llm::EventHandler callback -- the stream callback is portable across the supported phone toolchains, so
// the handler IS the stream, the same model the llm clients already use.

#include "blob.hpp"
#include "config/config.hpp"
#include "llm/client.hpp"
#include "llm/event.hpp"
#include "llm/mirothinker.hpp"   // ChatMessage, EventHandler

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mirobody {
namespace cache    { class Cache; }
namespace storage  { class Storage; }
namespace memory   { class Memory; }
namespace database { class Database; }
namespace chat {

//------------------------------------------------------------------------------
// Request
//------------------------------------------------------------------------------

// A file attached to the turn. Mirrors the file_infos BaselineAgent forwards to
// the provider (URL + MIME type), trimmed to what the C++ clients can use.
struct AgentFile {
    std::string filename;
    std::string mime_type;
    std::string file_key;
    std::string url;
    // Raw bytes when the file was uploaded inline this turn; empty for a
    // reference-only file (an earlier upload, reachable by url / file_key). A
    // shared handle, not a copy: the agent receives the request by const
    // reference and hands these same bytes to the provider client, so owning
    // them by value cost a full copy at both hops. See mirobody::Blob.
    Blob data;
};

// Everything an agent needs to produce a response. The flattened C++ analog of
// the Python generate_response(**kwargs) / ChatStreamRequest surface, carrying
// only the fields the current agents consume.
struct AgentRequest {
    std::string                   question;     // the latest user message text
    std::vector<llm::ChatMessage> messages;     // full conversation so far
    std::string                   provider;     // requested LLM provider name

    std::int64_t user_id = 0;                    // decoded raw row id; 0 => anonymous
    std::string  session_id;

    // Durable conversation thread (modern backends only).
    //   conversation_id  IN:  continue this server-side thread (the client echoes
    //                         the id it was told); 0 / unowned => start a new one.
    //                    OUT: set by persist_history to the thread this turn
    //                         landed in, so the dispatcher can tell the client.
    //   question_id      OUT: this turn's question messages.id, set by
    //                         persist_history so the streamed answer links back to it.
    std::int64_t conversation_id = 0;
    std::int64_t question_id     = 0;

    // Chat "currently for" subject: a care-circle member whose health the caller
    // may read. Validated against circle::can_read_health before it reaches the
    // agent; 0 / self => the caller's own data. Drives a system-prompt hint so
    // health questions default to this member (the family_health tool reads by id).
    std::int64_t subject_user_id = 0;

    std::string  language;                       // hint; empty => auto-detect
    std::string  accept_language;                // client's top Accept-Language tag (e.g. "fr-FR"); empty => none
    std::string timezone;                        // IANA zone; empty => default

    // Incognito ("privacy mode") turn: the client asked that this exchange leave
    // no trace. The dispatcher then skips durable-thread persistence (no
    // conversation id is surfaced) and withholds the memory handle (so the
    // remember tool is inert), and Chat::response skips the cache-backed
    // conversation memory. The turn still runs normally; only persistence is off.
    bool                     incognito = false;

    std::vector<AgentFile>   files;
    bool                     enable_mcp = true;
    std::vector<std::string> allowed_tools;
    std::vector<std::string> disallowed_tools;

    // Borrowed services for locally-executed tools (see mcp::ToolContext): the
    // per-user file index and the object store. Set by the dispatcher, which
    // owns them; null when unavailable. Carried here because the agent's tool
    // executor (a free function) only sees the threaded UserContext, not the
    // server's service objects.
    cache::Cache*       cache   = nullptr;
    storage::Storage*   storage = nullptr;
    memory::Memory*     memory  = nullptr;
    database::Database* db      = nullptr;   // relational store (chat-history tools)
};

//------------------------------------------------------------------------------
// Agent
//------------------------------------------------------------------------------

class Agent {
public:
    virtual ~Agent() = default;

    // Stream the response through `on_event`. Returning false from the handler
    // aborts the in-flight work. Errors are reported as Event{EventType::Error}
    // rather than thrown.
    virtual void generate_response(const AgentRequest&  req,
                                   const llm::EventHandler& on_event) = 0;
};

//------------------------------------------------------------------------------
// Registration
//------------------------------------------------------------------------------

// Provider-name -> client. The per-agent client store, built once from config.
using ClientMap = std::unordered_map<std::string, std::shared_ptr<llm::Client> >;

// Builds a fresh agent instance for one request (agents carry per-request
// identity/options, so the registry stores a factory, not an instance).
using AgentFactory = std::function<std::unique_ptr<Agent>(const AgentRequest&)>;

// Builds the agent's provider clients from configuration. The analog of the
// Python agent class's static load_llm_clients(). May be null (an agent that
// needs no clients of its own). Called once at startup.
using ClientLoader = std::function<ClientMap(const Config&)>;

struct AgentRegistration {
    std::string  name;          // e.g. "Baseline" ("Agent" suffix already dropped)
    bool         is_public;     // listed to end users vs. internal-only
    AgentFactory factory;
    ClientLoader load_clients;  // may be null
};

//------------------------------------------------------------------------------
// Registry
//------------------------------------------------------------------------------

class AgentRegistry {
public:
    // Register an agent. Returns true so it can initialize a file-scope static
    // (see MIROBODY_REGISTER_AGENT). A duplicate name logs a warning and is
    // ignored.
    bool add(const AgentRegistration& reg);

    const AgentRegistration* find(const std::string& name) const;

    // Registered agent names; `public_only` filters to user-facing agents.
    std::vector<std::string> names(bool public_only = false) const;

    // The first registered agent's name (registration order), or "" if none.
    // `public_only` filters to user-facing agents. Used as the default agent
    // when a request names none.
    std::string first_name(bool public_only = true) const;

    // "agent/provider" pairs for every agent that has at least one provider
    // client loaded (call after load_clients). Sorted. The analog of Python's
    // get_agents_with_llm_client_names; backs the /api/providers endpoint.
    std::vector<std::string> provider_names(bool public_only = true) const;

    std::size_t size() const { return agents_.size(); }

    // Build every agent's provider clients from `cfg`. Call once at startup,
    // after registration (i.e. after main() begins). Idempotent: a second call
    // rebuilds the store.
    void load_clients(const Config& cfg);

    // The client an agent should use for `provider`, or nullptr when the agent
    // or provider is unknown / has no client. The analog of Python's
    // get_llm_client_by_name.
    std::shared_ptr<llm::Client> client(const std::string& agent_name,
                                        const std::string& provider) const;

    // Instantiate an agent by name for `req`, or nullptr when unknown.
    std::unique_ptr<Agent> create(const std::string& name, const AgentRequest& req) const;

    // The default agent -- the DEFAULT_AGENT config value when it names a public
    // agent, else the first public agent (registration order), else "". This is the
    // agent whose providers are listed WITHOUT the "agent/" prefix and the one a
    // nameless / unrecognized request routes to. Set from config in load_clients.
    std::string default_agent_name(bool public_only = true) const;

    // Map a client-supplied agent token onto a real agent name. A token that
    // names no registered agent is treated as a bare *provider* under the default
    // agent -- which is how the prefix-less /api/providers list is routed: the
    // client submits the model as the "agent", and it lands here. When the token is
    // taken as a provider and `provider` was empty, `provider` is set to it. An
    // empty token also defaults to the default agent. Returns "" only when no agents
    // are registered. Old "agent/provider" submissions, whose token IS a real agent
    // name, are returned unchanged (so nothing about them breaks).
    std::string resolve_agent(const std::string& token, std::string& provider) const;

private:
    std::vector<AgentRegistration>               agents_;
    std::unordered_map<std::string, std::size_t> index_;
    std::unordered_map<std::string, ClientMap>   clients_;   // agent -> provider -> client
    std::string                                  default_agent_;   // DEFAULT_AGENT config (may be empty)
};

// Process-wide registry. A function-local static, constructed on the first
// add(), so cross-translation-unit init order is not a concern.
AgentRegistry& agent_registry();

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------

// Best-effort response-language instruction. A recognized Unicode script in
// `text` wins outright and yields a language name ("Japanese", "Korean",
// "Simplified Chinese", "Arabic", "Hebrew", "Russian"). Latin-script text asks
// the model to mirror the user's message. `accept_language` (the client's top
// Accept-Language tag, e.g. "fr-FR") is the fallback: it disambiguates a
// Latin-script message and supplies a language for an empty (upload-only) turn.
// Returns "" only when there's neither text nor an Accept-Language hint, so the
// caller drops the instruction and the model decides.
std::string detect_language(const std::string& text,
                            const std::string& accept_language = std::string());

//------------------------------------------------------------------------------
// Self-registration macro
//------------------------------------------------------------------------------

#define MIROBODY_AGENT_CONCAT_(a, b) a##b
#define MIROBODY_AGENT_CONCAT(a, b)  MIROBODY_AGENT_CONCAT_(a, b)

// Place at file scope in a res/agents/*.cpp after defining an AgentRegistration:
//     MIROBODY_REGISTER_AGENT(kBaselineAgent);
#define MIROBODY_REGISTER_AGENT(reg_expr)                                     \
    namespace {                                                               \
        const bool MIROBODY_AGENT_CONCAT(_agent_reg_, __LINE__) =             \
            ::mirobody::chat::agent_registry().add(reg_expr);                 \
    }

}}
