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

    // The caller's identity is established server-side from the JWT / personal
    // MCP secret; we don't echo the internal users PK back out. The session id
    // (the caller's own, opaque) is enough to confirm "you are authenticated".
    rapidjson::Document d;
    d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();

    d.AddMember("authenticated", true, a);
    d.AddMember("session_id",
                rapidjson::Value(user.session_id.c_str(),
                                 static_cast<rapidjson::SizeType>(user.session_id.size()), a),
                a);

    // When the turn is "currently for" a care-circle member (read-only access the
    // caller was granted), flag it so the model knows health questions default to
    // that member. We still don't echo any internal users PK -- this is a boolean,
    // and only health-read tools (family_health) actually act on the subject.
    if (user.subject_user_id > 0) {
        d.AddMember("acting_for_member", true, a);
    }

    return Result::ok(to_json(d));
}

//------------------------------------------------------------------------------

const Tool kWhoami = {
    "whoami",
    "Confirm the caller is authenticated and return their session identifier.",
    true,                                                     // auth
    {},                                                       // no parameters
    &whoami,
};

}   // namespace

MIROBODY_REGISTER_TOOL(kWhoami);
