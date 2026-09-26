#pragma once

// MCP tool registry.
//
// The C++ analog of the Python reference's mcp/tool.py. Python derived each
// tool's JSON Schema from runtime reflection over the function signature and
// docstring; C++17 has no standard reflection, so each tool declares its
// parameters in a small table (`std::vector<Param>`) and the registry expands
// that into the MCP `inputSchema` (and the OpenAI / Gemini function-descriptor
// variants). The declaration is explicit and checked at compile time, which
// trades a few lines per tool for the elimination of fragile type-string and
// docstring parsing.
//
// Discovery is compile-time, not runtime: every file under res/mcp_tools/ is
// globbed into the build and ends with a MIROBODY_REGISTER_TOOL(...) line that
// self-registers via a file-scope static. Drop a new .cpp in that directory,
// rebuild, and the tool is live. See CMakeLists.txt (the mcp_tools OBJECT
// library) for why the objects are fed straight into the final binary rather
// than buried in the mirobody_core archive.

#include "platform/log.hpp"   // C++17 floor static_assert + logging

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <rapidjson/document.h>

namespace mirobody {

// Forward declarations: a tool handler reaches these services through the
// ToolContext below. Pointers only, so tool.hpp stays decoupled from their
// definitions (and from the cache/storage/database translation units).
namespace cache    { class Cache; }
namespace storage  { class Storage; }
namespace database { class Database; }
namespace memory   { class Memory; }

namespace mcp {

//------------------------------------------------------------------------------
// Parameter declaration
//------------------------------------------------------------------------------

// JSON Schema scalar/aggregate kinds. Mirrors the type map in Python's
// mcp/tool.py::_parse_type (str->string, int->integer, float->number,
// bool->boolean, list->array, dict->object).
enum class Type { String, Integer, Number, Boolean, Array, Object };

// Readability aliases for the Param `required` flag at the call site.
const bool Required = true;
const bool Optional = false;

// One declared tool parameter. Has a user-provided constructor (so it is not
// an aggregate) to give `description` and `item` explicit defaults.
struct Param {
    std::string name;
    Type        type;
    bool        required;
    std::string description;
    Type        item;          // element type when `type == Type::Array`

    Param(std::string name_, Type type_, bool required_,
          std::string description_ = std::string(), Type item_ = Type::String)
        : name(std::move(name_))
        , type(type_)
        , required(required_)
        , description(std::move(description_))
        , item(item_) {}
};

//------------------------------------------------------------------------------
// Call context + result
//------------------------------------------------------------------------------

// Injected by the service for tools flagged `auth` (the analog of Python's
// `user_info` parameter). For non-auth tools user_id is 0 and session_id is
// empty, so authed() is false.
struct UserInfo {
    std::int64_t user_id = 0;     // the caller's row id; 0 == unauthenticated
    std::string  session_id;

    // The care-circle "currently for" subject: a member whose health the caller
    // was authorized to *read* this turn (resolved + access-checked in the chat
    // dispatcher; 0 == none / acting as self). Identity stays the caller's --
    // this only lets health-read tools (family_health) default to the subject so
    // "how is Mom doing?" works without the model restating the id. Tools that
    // write or expose other data must keep using `user_id`, never this.
    std::int64_t subject_user_id = 0;

    // A call is authenticated exactly when it carries a caller identity. Row
    // ids are positive, so a non-zero user_id marks an authenticated call.
    bool authed() const { return user_id > 0; }
};

// Services a tool handler may need beyond its arguments and caller identity.
// Whoever dispatches the call fills the handles it has (the MCP service passes
// its cache + object store; the in-process agent executor passes the request's);
// a handler uses only what it needs and tolerates a null (e.g. no object store
// configured). All pointers are borrowed and outlive the call.
struct ToolContext {
    cache::Cache*       cache;     // per-user indices (see transcode/)
    storage::Storage*   storage;   // object store for uploaded bytes
    database::Database* db;        // relational store
    memory::Memory*     memory;    // long-term memory store (see memory/)

    ToolContext(cache::Cache* c = nullptr, storage::Storage* s = nullptr,
                database::Database* d = nullptr, memory::Memory* m = nullptr)
        : cache(c), storage(s), db(d), memory(m) {}
};

// What a tool handler returns. Keeps rapidjson out of the handler signature:
// `json` is the JSON-encoded data on success, or a plain error message when
// `success` is false. The service wraps this into the MCP content/isError/
// structuredContent envelope.
struct Result {
    bool        success;
    std::string json;

    Result(bool ok, std::string payload)
        : success(ok), json(std::move(payload)) {}

    static Result ok(std::string json_data)  { return Result(true,  std::move(json_data)); }
    static Result error(std::string message) { return Result(false, std::move(message)); }
};

//------------------------------------------------------------------------------
// Typed argument accessor
//------------------------------------------------------------------------------

// Thin read-only view over the JSON-RPC `params.arguments` object. Replaces
// Python call_tool's kwargs binding: instead of the registry matching argument
// names to a signature, the handler pulls the values it declared, applying a
// default when a key is missing or the wrong type. Tolerant by construction --
// a non-object value (or absent arguments) yields defaults everywhere.
class Args {
public:
    explicit Args(const rapidjson::Value& v) : v_(&v) {}

    bool has(const char* key) const;

    std::string              str(const char* key, const std::string& def = std::string()) const;
    long long                integer(const char* key, long long def = 0) const;
    double                   number(const char* key, double def = 0.0) const;
    bool                     boolean(const char* key, bool def = false) const;
    std::vector<std::string> string_array(const char* key) const;

    // Escape hatch for arguments whose shape the typed getters do not cover.
    const rapidjson::Value& raw() const { return *v_; }

private:
    const rapidjson::Value* v_;
};

//------------------------------------------------------------------------------
// Tool definition
//------------------------------------------------------------------------------

// A registered tool. This is an aggregate record (no constructors or hidden
// initialization) so it can be brace-initialized positionally:
//
//     const Tool kEcho = {
//         "echo",
//         "Echo back the provided text.",
//         false,                                          // auth
//         { Param("text", Type::String, Required, "Text to echo") },
//         &echo_handler,
//     };
//
// The trailing `raw_input_schema` may be omitted; when non-empty it is parsed
// and used verbatim, overriding the generated schema (the analog of Python's
// custom `inputSchema` attribute branch) for the rare tool whose arguments are
// not flat scalars/arrays.
struct Tool {
    std::string                                                       name;
    std::string                                                       description;
    bool                                                              auth;
    std::vector<Param>                                                params;
    std::function<Result(const Args&, const UserInfo&, const ToolContext&)> handler;
    std::string                                                       raw_input_schema;
};

//------------------------------------------------------------------------------
// Registry
//------------------------------------------------------------------------------

class Registry {
public:
    // Register a tool. Returns true unconditionally so it can initialize a
    // file-scope static (see MIROBODY_REGISTER_TOOL). A duplicate name logs a
    // warning and is ignored.
    bool add(const Tool& tool);

    const Tool*              find(const std::string& name) const;
    std::vector<std::string> names() const;
    std::size_t              size() const { return tools_.size(); }

    // MCP `tools/list` result array, JSON-encoded.
    std::string tools_list_json() const;

    // LLM function-call descriptor variants, JSON-encoded. `style` is "openai"
    // (Chat Completions nested {type,function}) or "gemini" ({name,description,
    // parameters}); any other value falls back to the OpenAI simplified shape.
    //
    // `include_auth` false drops the auth-flagged tools from the descriptor: a
    // context whose turns run unauthenticated (the embedded mobile build today,
    // user_id 0) must not advertise tools that are guaranteed to answer
    // "Authentication required" -- the model calls them, burns a round trip,
    // and has to recover.
    std::string functions_json(const char* style, bool include_auth = true) const;

    // Dispatch a `tools/call`. `arguments` is the JSON-RPC params.arguments
    // value (may be a non-object, in which case the handler sees defaults).
    // `ctx` carries the services the handler may need (see ToolContext).
    // Never throws -- a handler exception becomes Result::error.
    Result call(const std::string&      name,
                const rapidjson::Value& arguments,
                const UserInfo&         user,
                const ToolContext&      ctx = ToolContext()) const;

private:
    std::vector<Tool>                            tools_;
    std::unordered_map<std::string, std::size_t> index_;
};

// Process-wide registry. A function-local static, so it is constructed on the
// first add() call -- which is exactly when the first tool's self-registration
// static runs -- sidestepping any cross-translation-unit init-order problem.
Registry& registry();

// Serialize any rapidjson value to a compact JSON string. Handy for tool
// handlers building their Result payload.
std::string to_json(const rapidjson::Value& v);

// Run a registered tool by name for an in-process tool loop (the OpenAI /
// Gemini client executors). `args_json` is the tool-call arguments object as a
// JSON string; empty or malformed input is tolerated (the tool sees defaults).
// `user` is forwarded so auth-scoped tools see the caller; `ctx` carries the
// services the handler may need (see ToolContext). Returns the tool's JSON
// payload on success, or "Error: <message>" on failure.
std::string run_mcp_tool(const std::string& name,
                         const std::string& args_json,
                         const UserInfo&    user,
                         const ToolContext& ctx = ToolContext());

//------------------------------------------------------------------------------
// Self-registration macro
//------------------------------------------------------------------------------

#define MIROBODY_MCP_CONCAT_(a, b) a##b
#define MIROBODY_MCP_CONCAT(a, b)  MIROBODY_MCP_CONCAT_(a, b)

// Place at file scope in a res/mcp_tools/*.cpp after defining a `Tool`:
//     MIROBODY_REGISTER_TOOL(kEcho);
#define MIROBODY_REGISTER_TOOL(tool_expr)                                    \
    namespace {                                                              \
        const bool MIROBODY_MCP_CONCAT(_mcp_reg_, __LINE__) =                \
            ::mirobody::mcp::registry().add(tool_expr);                      \
    }

}
}
