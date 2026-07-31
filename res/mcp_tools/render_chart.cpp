// MCP tool: render_chart. No authentication required.
//
// Lets an agent draw a chart in the chat UI. The model calls this with an
// Apache ECharts `option` object; the chat stream turns the call into a Chart
// event the frontend renders with echarts.setOption() (see the ChartFilter in
// src/chat/event/filters/chart.cpp).
//
// The tool itself only validates the option and returns a short acknowledgement
// — the option travels to the client out-of-band via the call arguments, so the
// model's context is not bloated by echoing the (potentially large) option back.

#include "mcp/tool.hpp"

#include <rapidjson/document.h>

namespace {

using namespace mirobody::mcp;

//------------------------------------------------------------------------------

Result render_chart(const Args& args, const UserInfo&, const ToolContext&) {
    const rapidjson::Value& v = args.raw();
    if (!v.IsObject() || !v.HasMember("option") || !v["option"].IsObject()) {
        return Result::error(
            "render_chart requires an 'option' object: a complete Apache ECharts "
            "option (e.g. {\"xAxis\":{...},\"yAxis\":{...},\"series\":[...]}).");
    }

    rapidjson::Document d;
    d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    d.AddMember("status", "ok", a);
    d.AddMember("message", "Chart rendered to the user.", a);
    return Result::ok(to_json(d));
}

//------------------------------------------------------------------------------

const Tool kRenderChart = {
    "render_chart",
    "Render a chart in the user's chat. Call this with a complete Apache ECharts "
    "`option` object to visualize data (line, bar, pie, scatter, etc.). Use it "
    "whenever a trend or comparison is clearer as a chart than as text or a table. "
    "Emit a THEME-NEUTRAL option: never set colors — each client applies its own "
    "light/dark palette, and any color in the option overrides it and breaks one of "
    "the two modes.",
    false,                                                    // auth
    {
        Param("option", Type::Object, Required,
              "A complete Apache ECharts option object, e.g. "
              "{\"title\":{\"text\":\"Weight\"},\"xAxis\":{\"type\":\"category\","
              "\"data\":[\"Mon\",\"Tue\"]},\"yAxis\":{\"type\":\"value\"},"
              "\"series\":[{\"type\":\"line\",\"data\":[70.1,70.4]}]}. "
              "Rules: (1) Omit every color key — no top-level `color` palette, no "
              "`backgroundColor`, no `itemStyle.color` / `lineStyle.color` / "
              "`textStyle.color`; the client themes the chart. (2) Give every series "
              "a `name`, and include a `legend` whenever there are 2+ series, so "
              "series are identifiable without relying on color. (3) One value axis "
              "only — never two y-scales; for a second measure of a different scale, "
              "call this tool again for a separate chart."),
        Param("title", Type::String, Optional,
              "Optional short caption shown alongside the chart."),
    },
    &render_chart,
};

}   // namespace

MIROBODY_REGISTER_TOOL(kRenderChart);
