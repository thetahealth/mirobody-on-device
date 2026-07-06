#pragma once

// The terminology Resolver: free text -> ranked standard codes.
//
// Lexical-first, deterministic. v1 does exact normalized-surface recall over
// the lexicon artifact; later milestones add alias/substring, span scan,
// fuzzy fallback, and the ported rerank stack. Holds a const reference to a
// Lexicon (which the caller owns and keeps alive).

#include <string>
#include <vector>

#include "indicator/lexicon.hpp"

namespace mirobody {
namespace indicator {

class Resolver {
public:
    explicit Resolver(const Lexicon& lex) : lex_(lex) {}

    // Resolve `term` to up to `top_k` ranked results (best first). `systems`,
    // when non-empty, restricts results to those code systems (names, e.g.
    // "LOINC"). Empty input or no match returns an empty vector.
    std::vector<ResolveResult> resolve(const std::string& term, int top_k = 5,
                                       const std::vector<std::string>& systems =
                                           std::vector<std::string>()) const;

private:
    const Lexicon& lex_;
};

}  // namespace indicator
}  // namespace mirobody
