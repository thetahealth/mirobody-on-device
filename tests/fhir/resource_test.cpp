#include "fhir/fhir.hpp"
#include "fhir/resource.hpp"

#include <rapidjson/document.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using mirobody::fhir::is_valid_fhir_id;
using mirobody::fhir::is_well_formed_resource_type;
using mirobody::fhir::is_supported_fhir_version;
using mirobody::fhir::validate_resource;
using mirobody::fhir::ValidationIssue;

namespace {
std::vector<ValidationIssue> validate_json(const std::string& json, std::string* type = nullptr,
                                           std::string* id = nullptr) {
    rapidjson::Document d;
    d.Parse(json.c_str(), json.size());
    REQUIRE_FALSE(d.HasParseError());
    return validate_resource(d, type, id);
}
}  // namespace

//------------------------------------------------------------------------------
// primitives
//------------------------------------------------------------------------------

TEST_CASE("fhir version + type + id primitives", "[fhir][resource]") {
    REQUIRE(is_supported_fhir_version("4.0.1"));
    REQUIRE(is_supported_fhir_version("4.3.0"));
    REQUIRE_FALSE(is_supported_fhir_version("3.0.2"));

    REQUIRE(is_well_formed_resource_type("Observation"));
    REQUIRE(is_well_formed_resource_type("MedicationRequest"));
    REQUIRE_FALSE(is_well_formed_resource_type("observation"));  // must start uppercase
    REQUIRE_FALSE(is_well_formed_resource_type(""));

    REQUIRE(is_valid_fhir_id("abc-123.4"));
    REQUIRE_FALSE(is_valid_fhir_id(""));
    REQUIRE_FALSE(is_valid_fhir_id("has space"));
    REQUIRE_FALSE(is_valid_fhir_id(std::string(65, 'a')));  // > 64 chars
}

//------------------------------------------------------------------------------
// validate_resource
//------------------------------------------------------------------------------

TEST_CASE("a well-formed resource validates and yields type + id", "[fhir][resource]") {
    std::string type, id;
    auto issues = validate_json(
        "{\"resourceType\":\"Observation\",\"id\":\"abc\",\"status\":\"final\"}", &type, &id);
    REQUIRE(issues.empty());
    REQUIRE(type == "Observation");
    REQUIRE(id == "abc");
}

TEST_CASE("missing resourceType is an error", "[fhir][resource]") {
    auto issues = validate_json("{\"status\":\"final\"}");
    REQUIRE_FALSE(issues.empty());
}

TEST_CASE("contained resources are rejected", "[fhir][resource]") {
    auto issues = validate_json(
        "{\"resourceType\":\"Observation\",\"contained\":[{\"resourceType\":\"Patient\"}]}");
    bool found = false;
    for (size_t i = 0; i < issues.size(); ++i)
        if (issues[i].diagnostics.find("contained") != std::string::npos) found = true;
    REQUIRE(found);
}

TEST_CASE("top-level null field is rejected", "[fhir][resource]") {
    auto issues = validate_json("{\"resourceType\":\"Observation\",\"value\":null}");
    bool found = false;
    for (size_t i = 0; i < issues.size(); ++i)
        if (issues[i].diagnostics.find("null") != std::string::npos) found = true;
    REQUIRE(found);
}

TEST_CASE("invalid id in body is rejected", "[fhir][resource]") {
    auto issues = validate_json("{\"resourceType\":\"Observation\",\"id\":\"bad id!\"}");
    REQUIRE_FALSE(issues.empty());
}

TEST_CASE("a non-object body is rejected", "[fhir][resource]") {
    auto issues = validate_json("[1,2,3]");
    REQUIRE_FALSE(issues.empty());
}
