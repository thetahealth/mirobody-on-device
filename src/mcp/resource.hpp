#pragma once

// MCP resource registry.
//
// The resource analog of mcp/tool.hpp. Where a tool is an action the model
// *calls* (tools/call), a resource is a named blob the client *reads*
// (resources/read) after discovering it via resources/list. Both follow the
// same compile-time self-registration pattern: every file under
// res/mcp_resources/ declares a `Resource` and ends with a
// MIROBODY_REGISTER_RESOURCE(...) line, the objects are fed straight into the
// final binary (see CMakeLists.txt), and the file-scope static wires the
// resource into the process-wide registry at startup. Drop a new .cpp in that
// directory, rebuild, and the resource is live.
//
// A resource is addressed by an opaque `uri` (e.g. "info://server" or
// "file:///guides/onboarding.md"). The handler produces the bytes on demand --
// resources are not stored, they are computed per read, so a resource can front
// dynamic data (the caller's profile, a generated report) just as easily as a
// static document.

#include "mcp/tool.hpp"   // UserInfo, to_json, the self-registration macro idiom

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mirobody { namespace mcp {

//------------------------------------------------------------------------------
// Read result
//------------------------------------------------------------------------------

// What a resource handler returns. Mirrors mcp::Result, but a resource read
// yields either UTF-8 text or binary bytes (base64-encoded by the handler),
// distinguished by `is_blob`. On failure `content` carries a plain message.
struct ResourceResult {
    bool        success;
    std::string content;     // text payload, or base64 bytes when is_blob
    bool        is_blob;     // false -> emit as MCP "text", true -> "blob"
    std::string mime_type;   // per-read mime type; empty -> use Resource.mime_type

    ResourceResult(bool ok, std::string payload, bool blob = false,
                   std::string mime = std::string())
        : success(ok)
        , content(std::move(payload))
        , is_blob(blob)
        , mime_type(std::move(mime)) {}

    // Text content. `mime` overrides the resource's declared mimeType when set.
    static ResourceResult text(std::string body, std::string mime = std::string()) {
        return ResourceResult(true, std::move(body), false, std::move(mime));
    }
    // Binary content; `b64` must already be base64-encoded.
    static ResourceResult blob(std::string b64, std::string mime = std::string()) {
        return ResourceResult(true, std::move(b64), true, std::move(mime));
    }
    static ResourceResult error(std::string message) {
        return ResourceResult(false, std::move(message));
    }
};

//------------------------------------------------------------------------------
// Resource definition
//------------------------------------------------------------------------------

// A registered resource. Like Tool, this is an aggregate record (no constructors)
// so it can be brace-initialized positionally:
//
//     const Resource kServerInfo = {
//         "info://server",                  // uri (the discovery + read key)
//         "Server info",                    // name
//         "Identity and capabilities ...",  // description
//         "application/json",               // mimeType
//         false,                            // auth
//         &server_info_handler,
//     };
//
// When `auth` is true the service resolves the caller's identity (bearer JWT or
// personal-MCP secret) before dispatch, exactly as for an auth-flagged tool,
// and hands it in via UserInfo; an unauthenticated read gets a 401.
struct Resource {
    std::string                                          uri;
    std::string                                          name;
    std::string                                          description;
    std::string                                          mime_type;
    bool                                                 auth;
    std::function<ResourceResult(const UserInfo&)>       handler;
};

//------------------------------------------------------------------------------
// Registry
//------------------------------------------------------------------------------

class ResourceRegistry {
public:
    // Register a resource. Returns true unconditionally so it can initialize a
    // file-scope static (see MIROBODY_REGISTER_RESOURCE). A duplicate uri logs a
    // warning and is ignored.
    bool add(const Resource& resource);

    const Resource*          find(const std::string& uri) const;
    std::vector<std::string> uris() const;
    std::size_t              size() const { return resources_.size(); }

    // MCP `resources/list` result array, JSON-encoded.
    std::string resources_list_json() const;

    // Dispatch a `resources/read`. Never throws -- a handler exception becomes
    // ResourceResult::error.
    ResourceResult read(const std::string& uri, const UserInfo& user) const;

private:
    std::vector<Resource>                        resources_;
    std::unordered_map<std::string, std::size_t> index_;
};

// Process-wide resource registry. A function-local static, constructed on the
// first add() call -- i.e. when the first resource's self-registration static
// runs -- sidestepping cross-translation-unit init-order issues.
ResourceRegistry& resource_registry();

//------------------------------------------------------------------------------
// Self-registration macro
//------------------------------------------------------------------------------

// Place at file scope in a res/mcp_resources/*.cpp after defining a `Resource`:
//     MIROBODY_REGISTER_RESOURCE(kServerInfo);
#define MIROBODY_REGISTER_RESOURCE(resource_expr)                            \
    namespace {                                                              \
        const bool MIROBODY_MCP_CONCAT(_mcp_res_reg_, __LINE__) =           \
            ::mirobody::mcp::resource_registry().add(resource_expr);         \
    }

}
}
