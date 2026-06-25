// MCP tool: recall_memory. Authentication required.
//
// Surfaces the long-term memory store (src/memory/) to an agent / MCP client:
// returns the caller's most relevant stored memories for a query, so the model
// can ground a turn in what it has learned about the user before. Reaches the
// store through the ToolContext the dispatcher / MCP service threads in; memory
// may be disabled (ctx.memory null), reported as a clean tool error.

#include "mcp/tool.hpp"
#include "memory/memory.hpp"

#include <rapidjson/document.h>

#include <string>
#include <vector>

namespace {

using namespace mirobody::mcp;

// String -> a freshly-allocated rapidjson string Value (copies into `a`).
rapidjson::Value str_val(const std::string& s, rapidjson::Document::AllocatorType& a) {
    return rapidjson::Value(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), a);
}

//------------------------------------------------------------------------------

Result recall_memory(const Args& args, const UserInfo& user, const ToolContext& ctx) {
    if (!user.authed()) {
        return Result::error("Authentication required");
    }
    if (ctx.memory == nullptr) {
        return Result::error("Long-term memory is not enabled on this server");
    }

    const std::string query = args.str("query");
    if (query.empty()) {
        return Result::error("query is required");
    }
    const int top_k = static_cast<int>(args.integer("top_k", 0));   // 0 => backend default

    std::string err;
    const std::vector<mirobody::memory::Record> hits =
        ctx.memory->recall(user.user_id, query, top_k, &err);
    if (!err.empty()) {
        return Result::error(err);
    }

    rapidjson::Document d;
    d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();

    rapidjson::Value arr(rapidjson::kArrayType);
    for (std::size_t i = 0; i < hits.size(); ++i) {
        const mirobody::memory::Record& h = hits[i];
        rapidjson::Value o(rapidjson::kObjectType);
        o.AddMember("id",         rapidjson::Value(h.id), a);
        o.AddMember("text",       str_val(h.text, a), a);
        o.AddMember("kind",       str_val(h.kind, a), a);
        o.AddMember("score",      rapidjson::Value(h.score), a);
        o.AddMember("created_at", rapidjson::Value(h.created_at), a);
        arr.PushBack(o, a);
    }
    d.AddMember("memories", arr, a);

    return Result::ok(to_json(d));
}

//------------------------------------------------------------------------------

const Tool kRecallMemory = {
    "recall_memory",
    "Recall the user's most relevant long-term memories (stored facts, preferences, "
    "and past context) for a query. Call this when you need background about the user "
    "that isn't in the current conversation.",
    true,                                                     // auth
    { Param("query", Type::String,  Required, "What to search the user's memories for"),
      Param("top_k", Type::Integer, Optional, "Maximum number of memories to return (default 5)") },
    &recall_memory,
};

}   // namespace

MIROBODY_REGISTER_TOOL(kRecallMemory);
