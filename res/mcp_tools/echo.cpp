// Example MCP tool: echo. No authentication required.
//
// Demonstrates the drop-a-file workflow: this .cpp is globbed into the build
// by CMakeLists.txt and self-registers via MIROBODY_REGISTER_TOOL at the
// bottom. Adding another tool is the same pattern in a new file.

#include "mcp/tool.hpp"

#include <rapidjson/document.h>

namespace {

using namespace mirobody::mcp;

//------------------------------------------------------------------------------

Result echo(const Args& args, const UserInfo&, const ToolContext&) {
    const std::string text = args.str("text");

    rapidjson::Document d;
    d.SetObject();
    rapidjson::Document::AllocatorType& a = d.GetAllocator();
    d.AddMember("echo",
                rapidjson::Value(text.c_str(),
                                 static_cast<rapidjson::SizeType>(text.size()), a),
                a);

    return Result::ok(to_json(d));
}

//------------------------------------------------------------------------------

const Tool kEcho = {
    "echo",
    "Echo back the provided text.",
    false,                                                    // auth
    { Param("text", Type::String, Required, "Text to echo back") },
    &echo,
};

}   // namespace

MIROBODY_REGISTER_TOOL(kEcho);
