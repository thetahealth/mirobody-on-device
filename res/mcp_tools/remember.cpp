// MCP tool: remember. Authentication required.
//
// Stores a durable fact about the caller in the long-term memory store
// (src/memory/), so a later turn can recall_memory it. This is the explicit
// capture path -- the model decides what is worth keeping -- complementing any
// future automatic extraction. Reaches the store through the ToolContext the
// dispatcher / MCP service threads in; memory may be disabled (ctx.memory null),
// reported as a clean tool error.

#include "mcp/tool.hpp"
#include "memory/memory.hpp"

#include <rapidjson/document.h>

#include <string>

namespace {

using namespace mirobody::mcp;

//------------------------------------------------------------------------------

Result remember(const Args& args, const UserInfo& user, const ToolContext& ctx) {
    if (!user.authed()) {
        return Result::error("Authentication required");
    }
    if (ctx.memory == nullptr) {
        return Result::error("Long-term memory is not enabled on this server");
    }

    const std::string text = args.str("text");
    if (text.empty()) {
        return Result::error("text is required");
    }

    mirobody::memory::RememberInput in;
    in.user_id    = user.user_id;
    in.text       = text;
    in.kind       = args.str("kind", "fact");
    in.session_id = user.session_id;
    if (in.kind.empty()) in.kind = "fact";

    std::string err;
    const std::int64_t id = ctx.memory->remember(in, &err);
    if (!err.empty()) {
        return Result::error(err);
    }

    rapidjson::Document d;
    d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    d.AddMember("id",     rapidjson::Value(id), a);
    d.AddMember("stored", rapidjson::Value(id > 0), a);

    return Result::ok(to_json(d));
}

//------------------------------------------------------------------------------

const Tool kRemember = {
    "remember",
    "Save a durable fact about the user to long-term memory (e.g. a stable preference, "
    "goal, or personal detail worth recalling in future conversations). Write it as a "
    "self-contained sentence. Do not store secrets or transient small talk.",
    true,                                                     // auth
    { Param("text", Type::String, Required, "The fact to remember, as a self-contained sentence"),
      Param("kind", Type::String, Optional, "Category tag: fact | preference | episode (default fact)") },
    &remember,
};

}   // namespace

MIROBODY_REGISTER_TOOL(kRemember);
