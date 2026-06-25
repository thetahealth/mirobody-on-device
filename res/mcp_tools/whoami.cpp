// Example MCP tool: whoami. Authentication required.
//
// `auth = true` is the C++ analog of declaring a `user_info` parameter in the
// Python reference: the service resolves the caller's identity (JWT / personal
// MCP secret) before dispatch and hands it in via UserInfo. A non-flat result
// shape would use the raw_input_schema escape hatch; this one is flat.

#include "mcp/tool.hpp"

#include <rapidjson/document.h>

namespace {

using namespace mirobody::mcp;

//------------------------------------------------------------------------------

Result whoami(const Args&, const UserInfo& user, const ToolContext&) {
    if (!user.authed()) {
        return Result::error("Not authenticated");
    }

    rapidjson::Document d;
    d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();

    d.AddMember("user_id", rapidjson::Value(user.user_id), a);
    d.AddMember("session_id",
                rapidjson::Value(user.session_id.c_str(),
                                 static_cast<rapidjson::SizeType>(user.session_id.size()), a),
                a);

    return Result::ok(to_json(d));
}

//------------------------------------------------------------------------------

const Tool kWhoami = {
    "whoami",
    "Return the authenticated caller's user and session identifiers.",
    true,                                                     // auth
    {},                                                       // no parameters
    &whoami,
};

}   // namespace

MIROBODY_REGISTER_TOOL(kWhoami);
