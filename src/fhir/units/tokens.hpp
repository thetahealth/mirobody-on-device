#pragma once

// Morpheme + alias tables — source of truth for unit normalization.
//
// C++11 port of mirobody/indicator/fhir/units/tokens.py. Two layers:
//   - morphemes() : atomic tokens consumed by the tokenize-compose path
//                   ("Millimol" + "/" + "L" -> "mmol/L").
//   - aliases()   : full-string mappings looked up directly without composing
//                   ("mmHg" -> "mm[Hg]").
//
// Each entry maps a canonical UCUM unit to a list of input variants. Variants
// are pre-cleaned at load time (NFKC + symbol fold) so source spelling can
// match what users actually type. Order is preserved as an ordered vector
// because normalize.cpp's inverted lookup is "last writer wins" on collisions,
// matching the Python dict build order.
//
// This file is UTF-8; the variants span en, zh-CN, zh-TW, ja, ko, ru, de, fr, es.

#include <string>
#include <utility>
#include <vector>

namespace mirobody { namespace fhir { namespace units {

typedef std::vector<std::pair<std::string, std::vector<std::string> > > TokenTable;

const TokenTable& morphemes();
const TokenTable& aliases();

}}}  // namespace mirobody::fhir::units
