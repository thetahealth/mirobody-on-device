// Example MCP resource: info://server. No authentication required.
//
// The resource analog of res/mcp_tools/whoami.cpp. A client discovers it via
// resources/list and fetches it with resources/read; the handler computes the
// JSON body on demand. This one is static (server identity + advertised
// capabilities), but a handler may read `user` and return per-caller data the
// same way an auth-flagged tool does -- set `auth = true` and the service
// resolves the caller's identity before dispatch.

#include "mcp/resource.hpp"

#include <rapidjson/document.h>

namespace {

using namespace mirobody::mcp;

//------------------------------------------------------------------------------

ResourceResult server_info(const UserInfo&) {
    rapidjson::Document d;
    d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();

    d.AddMember("name", "Theta MCP Server", a);
    d.AddMember("description",
                "Mirobody health assistant MCP endpoint. Tools are listed via "
                "tools/list; readable resources via resources/list.",
                a);

    rapidjson::Value caps(rapidjson::kArrayType);
    caps.PushBack("tools", a);
    caps.PushBack("resources", a);
    d.AddMember("capabilities", caps, a);

    return ResourceResult::text(to_json(d), "application/json");
}

//------------------------------------------------------------------------------

const Resource kServerInfo = {
    "info://server",
    "Server info",
    "Identity and advertised capabilities of this MCP server.",
    "application/json",
    false,                                                    // auth
    &server_info,
};

}   // namespace

MIROBODY_REGISTER_RESOURCE(kServerInfo);
