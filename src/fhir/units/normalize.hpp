#pragma once

// Free-text unit string -> canonical UCUM.
//
// C++11 port of mirobody/indicator/fhir/units/normalize.py. Pipeline:
//
//   raw input
//     -> unicode NFKC-lite  (full-width forms, superscripts)
//        -> symbol fold      (micro sign / multiplication / degree, drop spaces)
//           -> flat alias lookup  -> canonical UCUM
//              -> if miss: tokenize-compose via morphemes
//
// We deliberately do NOT lowercase: UCUM is case-sensitive ("mg" = milligram,
// "MG" = megagram). Case variants live explicitly in the alias layer.
//
// NFKC note: the reference uses Python's full unicodedata NFKC. To stay
// dependency-free (no ICU) this port implements a targeted NFKC-lite covering
// the compatibility characters that actually appear in clinical unit strings —
// full-width ASCII (U+FF01..FF5E), the ideographic space, and superscript
// digits. Micro sign / Greek mu and the other folds are handled in the symbol
// step. See clean() in normalize.cpp.

#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace fhir { namespace units {

// Parsed result of a value+unit free-text string. Each field is independently
// optional, mirroring the Python ParsedQuantity:
//
//   "5.6 mmol/L" -> {comparator:"",  has_value:true,  value:5.6, unit:"mmol/L"}
//   "<5.6"        -> {comparator:"<", has_value:true,  value:5.6, unit:""}
//   "mmol/L"      -> {comparator:"",  has_value:false,            unit:"mmol/L"}
//   "negative"    -> {comparator:"",  has_value:false,            unit:""}
struct ParsedQuantity {
    std::string comparator;        // "", "<", "<=", ">", ">=", "~", "≤", "≥", "≈"
    bool        has_value = false; // false == Python None
    double      value = 0.0;
    std::string unit;              // "" == Python None

    ParsedQuantity() {}
    ParsedQuantity(const std::string& c, bool hv, double v, const std::string& u)
        : comparator(c), has_value(hv), value(v), unit(u) {}
};

// Canonicalize a free-text unit string to UCUM. Returns "" when unrecognized.
// Lookup: cleaned input -> alias -> annotation strip -> tokenize-compose, with a
// last-resort leading-value strip ("90" in "90次每分钟", "<5.6" in "<5.6 mg/dL").
std::string normalize_unit(const std::string& text);

// Parse a free-text "value + unit" string into its components. Lookup order:
//   1. Whole input as a unit (preserves canonicals that start with digits).
//   2. Value at start (with optional comparator): "<5.6 mg/dL".
//   3. Value anywhere: "每分钟90次" (Chinese SVO), only if the leftover resolves.
ParsedQuantity parse_value_unit(const std::string& text);

// Find every (value, UCUM unit) pair in `text` belonging to a dose-relevant
// family (Mass / Vol / CCnt / MCnt / Arb). Deduped, in encounter order. Used by
// the resolver's dose-aware rerank — concentration units (mmol/L etc.) are
// intentionally excluded since the PROPERTY axis already covers them.
std::vector<std::pair<double, std::string> > scan_value_units(const std::string& text);

}}}  // namespace mirobody::fhir::units
