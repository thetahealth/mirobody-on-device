// MCP tool: ask_user. No authentication required.
//
// Puts a multiple-choice question to the user and BLOCKS until they answer, so the
// model can ask and then keep working inside one turn rather than ending its turn
// and treating the reply as a new question. The chat stream turns the call into an
// Ask event the frontend renders as native choices (see AskFilter in
// src/chat/event/filter/ask.cpp); the answer comes back here as this tool's result.
//
// Registered wherever the agent loop runs IN-PROCESS. Behind an HTTP server it is
// left out and the model asks in prose instead -- see the note in mcp/ask.hpp on why
// that boundary exists and why it costs no second protocol.
//
// Ask when the answer genuinely changes what you do next. A model that asks about
// everything is worse than one that picks a sensible default and says which it took.

#include "mcp/ask.hpp"
#include "mcp/tool.hpp"

#include <rapidjson/document.h>

namespace {

using namespace mirobody::mcp;

// Long enough that a user who is reading the question, or who switched apps to
// check something, still gets to answer; short enough that a turn abandoned
// mid-question does not park a thread for the rest of the session.
const int kTimeoutSeconds = 300;

Result ask_user(const Args& args, const UserInfo&, const ToolContext&) {
    const rapidjson::Value& v = args.raw();
    if (!v.IsObject() || !v.HasMember("question") || !v["question"].IsString()
        || v["question"].GetStringLength() == 0) {
        return Result::error("ask_user requires a non-empty 'question' string.");
    }
    if (!v.HasMember("options") || !v["options"].IsArray() || v["options"].Size() < 2) {
        return Result::error(
            "ask_user requires an 'options' array of at least 2 choices. With fewer "
            "than two there is nothing to choose: state what you are doing instead.");
    }

    const std::string answer = AskBroker::instance().wait(kTimeoutSeconds);

    rapidjson::Document d;
    d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();

    if (answer.empty()) {
        // Timed out, or the turn was cancelled. Reported as a fact the model can act
        // on rather than an error: the right move is usually to proceed on a stated
        // default, and an `isError` result invites it to apologize instead.
        d.AddMember("answered", false, a);
        d.AddMember("reason", "The user did not answer.", a);
        d.AddMember("guidance",
                    "Continue with the most reasonable default and say which one you "
                    "took, so they can correct it.", a);
        return Result::ok(to_json(d));
    }

    // The client's answer is already JSON ({"selected":[...],"other":"..."}); embed
    // it rather than re-encoding it as a string the model has to unquote.
    rapidjson::Document parsed;
    d.AddMember("answered", true, a);
    if (!parsed.Parse(answer.c_str()).HasParseError()) {
        d.AddMember("answer", rapidjson::Value(parsed, a), a);
    } else {
        d.AddMember("answer",
                    rapidjson::Value(answer.c_str(),
                                     static_cast<rapidjson::SizeType>(answer.size()), a), a);
    }
    return Result::ok(to_json(d));
}

//------------------------------------------------------------------------------

const Tool kAskUser = {
    "ask_user",
    "Ask the user a multiple-choice question and wait for their answer, without "
    "ending your turn. Use it when the answer changes what you do next and you "
    "cannot infer it -- which of several readings they meant, which of several "
    "actions to take. Do NOT use it for things you can decide yourself, for "
    "confirmation of something harmless, or to ask permission to answer: picking a "
    "sensible default and naming it is better than a question. One question at a "
    "time, and keep working once you have the answer.",
    false,                                                    // auth
    {
        Param("question", Type::String, Required,
              "The question, in the user's language. One sentence, specific enough "
              "to answer without re-reading the conversation."),
        Param("options", Type::Array, Required,
              "2-4 choices, each a short label the user can pick. Make them "
              "genuinely different -- options that lead to the same work are noise. "
              "The client always offers a free-text alternative, so no 'other' entry "
              "is needed.", Type::String),
        Param("multi_select", Type::Boolean, Optional,
              "True when several choices may be picked together. Default false."),
    },
    &ask_user,
};

}   // namespace

MIROBODY_REGISTER_TOOL(kAskUser);
