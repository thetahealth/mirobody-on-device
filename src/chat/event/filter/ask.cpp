#include "chat/event/filter/ask.hpp"

#include "chat/event/event.hpp"
#include "mcp/ask.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace mirobody { namespace chat {

namespace {

const char* const kAskUserTool = "ask_user";

std::string serialize(const rapidjson::Value& v) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    v.Accept(w);
    return std::string(buf.GetString(), buf.GetSize());
}

// Pull the question and choices out of an ask_user call's arguments. Returns false
// when they are not there -- a malformed call falls through as an ordinary tool step
// rather than producing a question with nothing to pick.
bool extract_ask(const std::string& args_json,
                 std::string& question, std::string& spec_json) {
    rapidjson::Document d;
    if (d.Parse(args_json.c_str()).HasParseError()) return false;

    // Some clients double-encode the arguments as a JSON string.
    rapidjson::Document inner;
    if (d.IsString()) {
        if (inner.Parse(d.GetString()).HasParseError()) return false;
        d.Swap(inner);
    }
    if (!d.IsObject()) return false;

    rapidjson::Value::ConstMemberIterator q = d.FindMember("question");
    if (q == d.MemberEnd() || !q->value.IsString()) return false;
    rapidjson::Value::ConstMemberIterator o = d.FindMember("options");
    if (o == d.MemberEnd() || !o->value.IsArray() || o->value.Size() == 0) return false;

    question.assign(q->value.GetString(), q->value.GetStringLength());

    rapidjson::Document spec;
    spec.SetObject();
    rapidjson::Document::AllocatorType& a = spec.GetAllocator();
    spec.AddMember("options", rapidjson::Value(o->value, a), a);

    rapidjson::Value::ConstMemberIterator m = d.FindMember("multi_select");
    spec.AddMember("multi_select",
                   m != d.MemberEnd() && m->value.IsBool() && m->value.GetBool(), a);

    spec_json = serialize(spec);
    return true;
}

}   // namespace

bool AskFilter::feed(const Event& e, const Sink& out) {
    const ToolEvent* t = dynamic_cast<const ToolEvent*>(&e);
    if (!t) return out(e);

    switch (t->phase()) {
        case ToolEvent::Phase::Title:
            if (t->content() == kAskUserTool && !t->tool_id().empty()) {
                ask_tools_.insert(t->tool_id());
                return true;   // suppress: an AskEvent replaces the triple
            }
            break;

        case ToolEvent::Phase::Arguments:
            if (ask_tools_.count(t->tool_id())) {
                std::string question, spec_json;
                if (extract_ask(t->content(), question, spec_json)) {
                    // Register BEFORE emitting. The handler that waits on this id
                    // runs on this thread the moment the round finishes assembling,
                    // and a waiter with no slot returns immediately with no answer.
                    mcp::AskBroker::instance().expect(t->tool_id());
                    return out(AskEvent(question, spec_json, t->tool_id()));
                }
                // Not a usable question: let the call show as a plain tool step.
                ask_tools_.erase(t->tool_id());
            }
            break;

        case ToolEvent::Phase::Detail:
            if (ask_tools_.count(t->tool_id())) {
                ask_tools_.erase(t->tool_id());
                return true;   // the result is the answer the client itself sent
            }
            break;
    }
    return out(e);
}

}}
