#pragma once

// Canonical UCUM unit -> LOINC PROPERTY family.
//
// C++11 port of mirobody/indicator/fhir/units/families.py. LOINC's PROPERTY
// axis is the dimensional family of a measurement (MCnc = mass concentration,
// SCnc = substance/molar concentration, NCnc = number concentration, ...). For
// a given analyte every PROPERTY family selects a fixed set of units — glucose
// in plasma is MCnc => mg/dL and SCnc => mmol/L, never the other way around. So
// once we know a measurement's unit, we know its PROPERTY family, and we can
// use that to filter / boost LOINC candidates whose PROPERTY column matches.
//
// Two tables:
//   - ucum_family()       : unique unit -> primary PROPERTY family.
//   - ambiguous_units()   : units that legitimately span multiple PROPERTYs
//                           (the worst offender is "%"). unit_family() returns
//                           the primary; unit_families() returns the full set.
//
// UCUM keys are case-sensitive. Bracketed units ([IU], [beth'U], [degF]) use
// the formal UCUM syntax — see ucum.org.

#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mirobody { namespace fhir { namespace units {

// UCUM_FAMILY in source (Python dict) insertion order. The normalize pipeline
// iterates this to auto-inject atomic UCUM canonicals into its lookup tables,
// and the order is the deterministic tie-break, so it is preserved verbatim.
const std::vector<std::pair<std::string, std::string> >& ucum_family_ordered();

// Flat unit -> primary family lookup, built from ucum_family_ordered().
const std::unordered_map<std::string, std::string>& ucum_family_map();

// Units that map to multiple PROPERTYs depending on context.
const std::unordered_map<std::string, std::set<std::string> >& ambiguous_units();

// Primary LOINC PROPERTY family for a canonical UCUM string. Returns "" for an
// unknown or empty unit. For ambiguous units the most-common family is
// returned (the ucum_family_map() entry) — use unit_families() for the full set.
std::string unit_family(const std::string& ucum);

// Every LOINC PROPERTY family compatible with a UCUM string. Empty set for an
// unknown unit; a single-element set for an unambiguous one; the full set from
// ambiguous_units() for "%", "mm[Hg]", etc.
std::set<std::string> unit_families(const std::string& ucum);

}}}  // namespace mirobody::fhir::units
