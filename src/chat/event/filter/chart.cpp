#include "chat/event/filter/chart.hpp"

#include "chat/event/event.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace mirobody { namespace chat {

namespace {

// The tool name an agent invokes to render a chart. Matches the tool definition
// in res/mcp_tools/render_chart.cpp.
const char* const kRenderChartTool = "render_chart";

// Serialize a rapidjson value to a compact JSON string.
std::string serialize(const rapidjson::Value& v) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    v.Accept(w);
    return std::string(buf.GetString(), buf.GetSize());
}

// Pull the ECharts option (and optional title) out of a render_chart call's
// arguments. `args_json` is the queryArguments content: a JSON object, or a JSON
// string wrapping one (some clients double-encode). Returns true and fills
// `option_json` / `title` on success.
bool extract_option(const std::string& args_json,
                    std::string& option_json, std::string& title) {
    rapidjson::Document d;
    if (d.Parse(args_json.c_str()).HasParseError()) return false;

    // Unwrap a double-encoded arguments string ("{\"option\": ...}").
    rapidjson::Document inner;
    if (d.IsString()) {
        if (inner.Parse(d.GetString()).HasParseError()) return false;
        d.Swap(inner);
    }
    if (!d.IsObject()) return false;

    // The model passes the option under "option"; tolerate the bare option
    // object as the arguments themselves.
    rapidjson::Value::ConstMemberIterator opt = d.FindMember("option");
    const rapidjson::Value& option = (opt != d.MemberEnd()) ? opt->value : d;
    if (!option.IsObject()) return false;

    rapidjson::Value::ConstMemberIterator t = d.FindMember("title");
    if (t != d.MemberEnd() && t->value.IsString()) {
        title.assign(t->value.GetString(), t->value.GetStringLength());
    }

    option_json = serialize(option);
    return true;
}

}   // namespace

bool ChartFilter::feed(const Event& e, const Sink& out) {
    const ToolEvent* t = dynamic_cast<const ToolEvent*>(&e);
    if (!t) return out(e);   // not a tool step -> pass through

    switch (t->phase()) {
        case ToolEvent::Phase::Title:
            if (t->content() == kRenderChartTool && !t->tool_id().empty()) {
                chart_tools_.insert(t->tool_id());
                return true;   // suppress: a ChartEvent replaces the triple
            }
            break;

        case ToolEvent::Phase::Arguments:
            if (chart_tools_.count(t->tool_id())) {
                std::string option_json, title;
                if (extract_option(t->content(), option_json, title)) {
                    return out(ChartEvent(title, option_json, t->tool_id()));
                }
                // Couldn't parse an option: stop treating this call as a chart
                // and let its events flow through unchanged.
                chart_tools_.erase(t->tool_id());
            }
            break;

        case ToolEvent::Phase::Detail:
            if (chart_tools_.count(t->tool_id())) {
                chart_tools_.erase(t->tool_id());
                return true;   // suppress the tool's acknowledgement result
            }
            break;
    }
    return out(e);
}

}}
