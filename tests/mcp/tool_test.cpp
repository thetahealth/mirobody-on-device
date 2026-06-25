#include "mcp/tool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <rapidjson/document.h>

#include <string>

using mirobody::mcp::Args;
using mirobody::mcp::Registry;
using mirobody::mcp::Result;
using mirobody::mcp::Tool;
using mirobody::mcp::UserInfo;
using mirobody::mcp::registry;

namespace {

//------------------------------------------------------------------------------

rapidjson::Document parse(const std::string& json) {
    rapidjson::Document d;
    REQUIRE_FALSE(d.Parse(json.c_str()).HasParseError());
    return d;
}

}   // namespace

//------------------------------------------------------------------------------
// Self-registration survives linking.
//
// This test binary links the src/mcp/tools/*.cpp objects via
// $<TARGET_OBJECTS:mcp_tools>. If those objects were instead buried in the
// mirobody_core static archive, the linker would discard the (unreferenced)
// self-registering translation units and these lookups would fail -- which is
// exactly the Option A failure mode the OBJECT-library wiring guards against.
//------------------------------------------------------------------------------

TEST_CASE("example tools self-register", "[mcp]") {
    REQUIRE(registry().find("echo")   != nullptr);
    REQUIRE(registry().find("whoami") != nullptr);
}

TEST_CASE("tools/list schema is generated from the Param table", "[mcp]") {
    rapidjson::Document tools = parse(registry().tools_list_json());
    REQUIRE(tools.IsArray());

    // Locate the echo descriptor.
    const rapidjson::Value* echo = nullptr;
    for (rapidjson::SizeType i = 0; i < tools.Size(); ++i) {
        if (std::string(tools[i]["name"].GetString()) == "echo") echo = &tools[i];
    }
    REQUIRE(echo != nullptr);

    const rapidjson::Value& schema = (*echo)["inputSchema"];
    REQUIRE(std::string(schema["type"].GetString()) == "object");
    REQUIRE(schema["properties"].HasMember("text"));
    REQUIRE(std::string(schema["properties"]["text"]["type"].GetString()) == "string");

    // "text" is Required, so it appears in the required array.
    REQUIRE(schema.HasMember("required"));
    REQUIRE(std::string(schema["required"][0].GetString()) == "text");
}

TEST_CASE("registry dispatches a non-auth tool", "[mcp]") {
    rapidjson::Document args = parse("{\"text\":\"hello\"}");
    Result r = registry().call("echo", args, UserInfo());

    REQUIRE(r.success);
    rapidjson::Document data = parse(r.json);
    REQUIRE(std::string(data["echo"].GetString()) == "hello");
}

TEST_CASE("auth tool sees the injected UserInfo", "[mcp]") {
    UserInfo user;
    user.user_id = 123;   // authed() derives from a positive user_id

    rapidjson::Document none = parse("{}");
    Result r = registry().call("whoami", none, user);

    REQUIRE(r.success);
    rapidjson::Document data = parse(r.json);
    REQUIRE(data["user_id"].GetInt64() == 123);
}

TEST_CASE("unknown tool is an error, not a crash", "[mcp]") {
    rapidjson::Document none = parse("{}");
    Result r = registry().call("does-not-exist", none, UserInfo());
    REQUIRE_FALSE(r.success);
}

TEST_CASE("Args applies defaults for missing / mistyped keys", "[mcp]") {
    rapidjson::Document d = parse("{\"s\":\"x\",\"n\":7,\"arr\":[\"a\",\"b\"],\"wrong\":\"nope\"}");
    Args a(d);

    REQUIRE(a.str("s") == "x");
    REQUIRE(a.str("missing", "fallback") == "fallback");
    REQUIRE(a.integer("n") == 7);
    REQUIRE(a.integer("wrong", 42) == 42);          // string where int expected -> default
    REQUIRE(a.string_array("arr").size() == 2);
    REQUIRE(a.string_array("arr")[1] == "b");
    REQUIRE(a.string_array("missing").empty());
}

TEST_CASE("summarize_conversation gates on auth and a configured store", "[mcp]") {
    using mirobody::mcp::ToolContext;

    REQUIRE(registry().find("summarize_conversation") != nullptr);

    rapidjson::Document args = parse("{\"summary\":\"diet check-in\"}");

    // Unauthenticated: rejected before touching any store.
    Result anon = registry().call("summarize_conversation", args, UserInfo());
    REQUIRE_FALSE(anon.success);

    // Authenticated but no relational store wired (ctx.db == nullptr): a clean
    // error, not a crash. A live-DB update path is exercised by the chat tests.
    UserInfo user;
    user.user_id = 7;
    Result no_db = registry().call("summarize_conversation", args, user, ToolContext());
    REQUIRE_FALSE(no_db.success);
}

TEST_CASE("raw_input_schema overrides the generated schema", "[mcp]") {
    Registry local;
    Tool t = {
        "raw_tool",
        "Tool with a hand-written schema.",
        false,
        {},
        nullptr,
        "{\"type\":\"object\",\"properties\":{\"custom\":{\"type\":\"number\"}}}",
    };
    local.add(t);

    rapidjson::Document tools = parse(local.tools_list_json());
    REQUIRE(tools.IsArray());
    REQUIRE(tools.Size() == 1);
    REQUIRE(tools[0]["inputSchema"]["properties"].HasMember("custom"));
    REQUIRE(std::string(tools[0]["inputSchema"]["properties"]["custom"]["type"].GetString()) == "number");
}
