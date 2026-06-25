#pragma once

// ChartFilter -- render_chart tool call -> ChartEvent.
//
// An agent renders a chart by calling the `render_chart` MCP tool (see
// res/mcp_tools/render_chart.cpp) with an Apache ECharts `option`. That surfaces
// in the event stream as the generic tool triple -- a ToolEvent for the title
// (the tool name), one for the arguments, one for the result. This filter watches
// that triple and, when the tool is `render_chart`, replaces it with a single
// ChartEvent carrying the option, suppressing the three raw tool events so the
// frontend sees a clean chart instead of tool plumbing.
//
// The option is taken from the call *arguments* (what the model authored), not
// the result, which is just an acknowledgement and is dropped. State is
// per-stream, so a fresh instance is used for each turn (see make_event_pipeline).

#include "chat/event/filter/filter.hpp"

#include <set>
#include <string>

namespace mirobody { namespace chat {

class ChartFilter : public EventFilter {
public:
    bool feed(const Event& e, const Sink& out) override;

private:
    // tool_ids of in-flight render_chart calls (recorded on the title event,
    // cleared once the call's events have been consumed).
    std::set<std::string> chart_tools_;
};

}}
