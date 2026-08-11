#pragma once

// AskFilter -- ask_user tool call -> AskEvent, and the rendezvous the answer needs.
//
// The same shape as ChartFilter: watch the generic tool triple (title, arguments,
// result), recognize one tool, and replace it with a typed event the frontend can
// render, suppressing the raw plumbing.
//
// It does one thing ChartFilter does not. A chart is fire-and-forget; a question is
// not, so on the arguments event this filter also registers the pending ask with
// mcp::AskBroker BEFORE emitting. That ordering is the whole contract: the arguments
// event is dispatched on the turn thread while llm/openai_chat.cpp is still
// assembling the round, and the tool handler -- which blocks on the broker -- runs
// immediately after on that same thread. Register late and the handler would wait on
// an id nobody registered.
//
// The tool_id doubles as the ask id, which is what lets the tool handler stay
// ignorant of ids entirely: the filter has it, the broker parks it on the thread,
// the handler just waits.
//
// The result event is suppressed like a chart's: it carries the answer, which the
// client is the one who sent.

#include "chat/event/filter/filter.hpp"

#include <set>
#include <string>

namespace mirobody { namespace chat {

class AskFilter : public EventFilter {
public:
    bool feed(const Event& e, const Sink& out) override;

private:
    std::set<std::string> ask_tools_;
};

}}
