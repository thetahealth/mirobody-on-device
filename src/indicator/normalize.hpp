#pragma once

// Surface-form normalization for the terminology resolver.
//
// Distinct from src/fhir/units/normalize (which canonicalizes UCUM units and
// is deliberately case-SENSITIVE). Here we fold to a case-insensitive,
// width-insensitive key so that a query surface and a vocabulary surface that
// differ only cosmetically hash to the same bucket. Both the build-time
// emitter (indexing vocabulary names/aliases) and the runtime resolver
// (normalizing the query) call this, so the two sides stay in lock-step.
//
// Pipeline (mirrors the Python alias-index normalization, NFKC + casefold):
//   decode UTF-8
//     -> NFKC-lite   (full-width ASCII, ideographic space, superscript digits)
//        -> casefold  (ASCII A-Z -> a-z)
//           -> collapse internal whitespace runs to one space + trim
//
// No ICU dependency — targeted folds only, same approach as units/normalize.

#include <string>

namespace mirobody {
namespace indicator {

// Normalize a surface form to its lookup key. Returns "" for empty/blank input.
std::string normalize(const std::string& text);

}  // namespace indicator
}  // namespace mirobody
