#include "mcp/resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <rapidjson/document.h>

#include <string>

using mirobody::mcp::Resource;
using mirobody::mcp::ResourceRegistry;
using mirobody::mcp::ResourceResult;
using mirobody::mcp::UserInfo;
using mirobody::mcp::resource_registry;

namespace {

rapidjson::Document parse(const std::string& json) {
    rapidjson::Document d;
    REQUIRE_FALSE(d.Parse(json.c_str()).HasParseError());
    return d;
}

}   // namespace

//------------------------------------------------------------------------------
// Self-registration survives linking, the same OBJECT-library guarantee the
// tools rely on (see tool_test.cpp).
//------------------------------------------------------------------------------

TEST_CASE("example resource self-registers", "[mcp][resource]") {
    REQUIRE(resource_registry().find("info://server") != nullptr);
}

TEST_CASE("resources/list descriptor carries uri/name/mimeType", "[mcp][resource]") {
    rapidjson::Document list = parse(resource_registry().resources_list_json());
    REQUIRE(list.IsArray());

    const rapidjson::Value* info = nullptr;
    for (rapidjson::SizeType i = 0; i < list.Size(); ++i) {
        if (std::string(list[i]["uri"].GetString()) == "info://server") info = &list[i];
    }
    REQUIRE(info != nullptr);
    REQUIRE(std::string((*info)["name"].GetString()) == "Server info");
    REQUIRE((*info).HasMember("description"));
    REQUIRE(std::string((*info)["mimeType"].GetString()) == "application/json");
}

TEST_CASE("resources/read dispatches to the handler", "[mcp][resource]") {
    UserInfo anon;   // info://server is not auth-flagged
    ResourceResult r = resource_registry().read("info://server", anon);
    REQUIRE(r.success);
    REQUIRE_FALSE(r.is_blob);
    REQUIRE(r.mime_type == "application/json");

    rapidjson::Document body = parse(r.content);
    REQUIRE(body.IsObject());
    REQUIRE(body.HasMember("name"));
    REQUIRE(body.HasMember("capabilities"));
}

TEST_CASE("reading an unknown uri fails cleanly", "[mcp][resource]") {
    UserInfo anon;
    ResourceResult r = resource_registry().read("info://does-not-exist", anon);
    REQUIRE_FALSE(r.success);
}

TEST_CASE("duplicate uri registration is ignored", "[mcp][resource]") {
    ResourceRegistry reg;
    Resource a = {"x://dup", "A", "", "", false,
                  [](const UserInfo&) { return ResourceResult::text("a"); }};
    Resource b = {"x://dup", "B", "", "", false,
                  [](const UserInfo&) { return ResourceResult::text("b"); }};
    REQUIRE(reg.add(a));
    REQUIRE(reg.add(b));   // returns true, but the second is ignored
    REQUIRE(reg.size() == 1);
    REQUIRE(reg.read("x://dup", UserInfo()).content == "a");
}
