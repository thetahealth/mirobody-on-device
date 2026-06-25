// MCP tool: summarize_conversation. Authentication required.
//
// Lets the model record a short summary of the conversation it is currently
// responding in. The dispatcher seeds a thin conversations row (keyed on the
// opening question) before streaming, with the question itself as a placeholder
// summary; this tool overwrites that row's `summary` with a model-written one,
// so chat-history listings show a meaningful title once the model understands
// the turn. The model does the summarizing -- the tool just persists the text it
// passes -- mirroring how `remember` persists a model-chosen fact.
//
// Reaches the relational store through the ToolContext the dispatcher / MCP
// service threads in; the chat module owns the backend-specific SQL (see
// chat::update_latest_conversation_summary). The store may be absent
// (ctx.db null), reported as a clean tool error.

#include "chat/chat.hpp"
#include "mcp/tool.hpp"
#include "platform/log.hpp"

#include <rapidjson/document.h>

#include <string>

namespace {

using namespace mirobody::mcp;

//------------------------------------------------------------------------------

Result summarize_conversation(const Args& args, const UserInfo& user, const ToolContext& ctx) {
    if (!user.authed()) {
        return Result::error("Authentication required");
    }
    if (ctx.db == nullptr) {
        return Result::error("Chat history store is not available");
    }

    std::string summary = args.str("summary");
    if (summary.empty()) {
        return Result::error("summary is required");
    }
    // Cap to the column's intent: a short title, not a transcript. Matches the
    // 200-char placeholder persist_history writes from the opening question.
    if (summary.size() > 200) summary.resize(200);

    mirobody::platform::log_debug("mcp[summarize_conversation]: user %lld, %lu chars",
                                  (long long)user.user_id, (unsigned long)summary.size());

    const bool updated = mirobody::chat::update_latest_conversation_summary(
        *ctx.db, user.user_id, summary);
    if (!updated) {
        return Result::error("No active conversation to summarize");
    }

    rapidjson::Document d;
    d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    d.AddMember("updated", rapidjson::Value(true), a);

    return Result::ok(to_json(d));
}

//------------------------------------------------------------------------------

const Tool kSummarizeConversation = {
    "summarize_conversation",
    "Record a concise summary (a short title, <= 200 chars) of the current "
    "conversation, used as its label in the user's chat history. Call this once "
    "you understand what the user wants -- it overwrites the placeholder taken "
    "from their opening question. Write the summary yourself; pass it as `summary`.",
    true,                                                     // auth
    { Param("summary", Type::String, Required,
            "A short, self-contained summary of the conversation (<= 200 chars).") },
    &summarize_conversation,
};

}   // namespace

MIROBODY_REGISTER_TOOL(kSummarizeConversation);
