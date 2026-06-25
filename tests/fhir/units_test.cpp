#include "fhir/units/families.hpp"
#include "fhir/units/normalize.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <set>
#include <string>

using mirobody::fhir::units::ParsedQuantity;
using mirobody::fhir::units::normalize_unit;
using mirobody::fhir::units::parse_value_unit;
using mirobody::fhir::units::scan_value_units;
using mirobody::fhir::units::unit_families;
using mirobody::fhir::units::unit_family;

namespace {
bool has_pair(const std::vector<std::pair<double, std::string> >& v, double val,
              const std::string& u) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].first == val && v[i].second == u) return true;
    return false;
}
}  // namespace

//------------------------------------------------------------------------------
// normalize_unit — canonical UCUM
//------------------------------------------------------------------------------

TEST_CASE("normalize_unit canonicalizes case + spelling variants", "[fhir][units]") {
    REQUIRE(normalize_unit("MG/DL") == "mg/dL");
    REQUIRE(normalize_unit("mg/dl") == "mg/dL");
    REQUIRE(normalize_unit("mg%") == "mg/dL");
    REQUIRE(normalize_unit("mmol/l") == "mmol/L");
    REQUIRE(normalize_unit("MMOL/L") == "mmol/L");
    REQUIRE(normalize_unit("mmHg") == "mm[Hg]");
    REQUIRE(normalize_unit("Torr") == "mm[Hg]");
    REQUIRE(normalize_unit("IU/L") == "[IU]/L");
    REQUIRE(normalize_unit("eGFR") == "mL/min/{1.73_m2}");
}

TEST_CASE("normalize_unit composes morphemes across languages", "[fhir][units]") {
    REQUIRE(normalize_unit(u8"毫摩尔每升") == "mmol/L");          // zh
    REQUIRE(normalize_unit("Millimol pro Liter") == "mmol/L");   // de
    REQUIRE(normalize_unit("Milligramm pro Deziliter") == "mg/dL");
}

TEST_CASE("normalize_unit handles unicode + annotations", "[fhir][units]") {
    REQUIRE(normalize_unit(u8"°C") == "Cel");
    REQUIRE(normalize_unit(u8"µg/L") == "ug/L");
    REQUIRE(normalize_unit(u8"10⁹/л") == "10*9/L");
    REQUIRE(normalize_unit("ug/g{creat}") == "ug/g");
    REQUIRE(normalize_unit("mL/min/{1.73_m2}") == "mL/min/{1.73_m2}");  // round-trips
}

TEST_CASE("normalize_unit strips a leading value as last resort", "[fhir][units]") {
    REQUIRE(normalize_unit("<5.6 mg/dL") == "mg/dL");
    REQUIRE(normalize_unit(u8"90次每分钟") == "/min");
}

TEST_CASE("normalize_unit returns empty for unrecognized input", "[fhir][units]") {
    REQUIRE(normalize_unit("negative") == "");
    REQUIRE(normalize_unit("") == "");
    REQUIRE(normalize_unit("mgL") == "");  // nonsense composition rejected
}

//------------------------------------------------------------------------------
// parse_value_unit — comparator / value / unit split
//------------------------------------------------------------------------------

TEST_CASE("parse_value_unit splits value and unit", "[fhir][units]") {
    ParsedQuantity q = parse_value_unit("5.6 mmol/L");
    REQUIRE(q.comparator == "");
    REQUIRE(q.has_value);
    REQUIRE(q.value == 5.6);
    REQUIRE(q.unit == "mmol/L");
}

TEST_CASE("parse_value_unit keeps the comparator", "[fhir][units]") {
    ParsedQuantity q = parse_value_unit("<5.6 mg/dL");
    REQUIRE(q.comparator == "<");
    REQUIRE(q.has_value);
    REQUIRE(q.value == 5.6);
    REQUIRE(q.unit == "mg/dL");

    ParsedQuantity ge = parse_value_unit(">=180 mmHg");
    REQUIRE(ge.comparator == ">=");
    REQUIRE(ge.unit == "mm[Hg]");
}

TEST_CASE("parse_value_unit treats leading - and + as sign not comparator", "[fhir][units]") {
    ParsedQuantity q = parse_value_unit("<5.6");
    REQUIRE(q.comparator == "<");
    REQUIRE(q.has_value);
    REQUIRE(q.value == 5.6);
    REQUIRE(q.unit == "");
}

TEST_CASE("parse_value_unit handles whole-input units and counts", "[fhir][units]") {
    ParsedQuantity u = parse_value_unit("mmol/L");
    REQUIRE_FALSE(u.has_value);
    REQUIRE(u.unit == "mmol/L");

    ParsedQuantity bpm = parse_value_unit(u8"90次每分钟");
    REQUIRE(bpm.has_value);
    REQUIRE(bpm.value == 90.0);
    REQUIRE(bpm.unit == "/min");

    ParsedQuantity steps = parse_value_unit(u8"600步");
    REQUIRE(steps.has_value);
    REQUIRE(steps.value == 600.0);
    REQUIRE(steps.unit == "{steps}");
}

TEST_CASE("parse_value_unit European decimal comma", "[fhir][units]") {
    ParsedQuantity q = parse_value_unit("5,6 mmol/L");
    REQUIRE(q.has_value);
    REQUIRE(q.value == 5.6);
    REQUIRE(q.unit == "mmol/L");
}

TEST_CASE("parse_value_unit refuses half-parsed results", "[fhir][units]") {
    ParsedQuantity q = parse_value_unit("negative");
    REQUIRE_FALSE(q.has_value);
    REQUIRE(q.unit == "");
}

//------------------------------------------------------------------------------
// unit_family / unit_families
//------------------------------------------------------------------------------

TEST_CASE("unit_family returns the primary LOINC PROPERTY", "[fhir][units]") {
    REQUIRE(unit_family("mmol/L") == "SCnc");
    REQUIRE(unit_family("mg/dL") == "MCnc");
    REQUIRE(unit_family("%") == "MFr");
    REQUIRE(unit_family("nope") == "");
}

TEST_CASE("unit_families returns the full ambiguous set", "[fhir][units]") {
    std::set<std::string> pct = unit_families("%");
    REQUIRE(pct.count("MFr") == 1);
    REQUIRE(pct.count("NFr") == 1);
    REQUIRE(pct.count("VFr") == 1);
    REQUIRE(pct.size() == 9);

    std::set<std::string> mmhg = unit_families("mm[Hg]");
    REQUIRE(mmhg.count("Pres") == 1);
    REQUIRE(mmhg.count("PPres") == 1);

    std::set<std::string> scnc = unit_families("mmol/L");
    REQUIRE(scnc.size() == 1);
    REQUIRE(scnc.count("SCnc") == 1);

    REQUIRE(unit_families("nope").empty());
}

//------------------------------------------------------------------------------
// scan_value_units — dose-family pairs in free text
//------------------------------------------------------------------------------

TEST_CASE("scan_value_units finds dose pairs and skips concentrations", "[fhir][units]") {
    REQUIRE(has_pair(scan_value_units("post 75 g glucose PO"), 75.0, "g"));
    REQUIRE(has_pair(scan_value_units("Glucose --2 hours post 100 g"), 100.0, "g"));
    REQUIRE(has_pair(scan_value_units(u8"OGTT 口服 75 克 葡萄糖"), 75.0, "g"));
    REQUIRE(scan_value_units("5.6 mmol/L").empty());  // SCnc is PROPERTY-axis territory
    REQUIRE(scan_value_units("2 hours post").empty());
}
