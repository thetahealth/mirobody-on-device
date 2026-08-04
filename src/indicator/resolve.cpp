#include "indicator/resolve.hpp"

#include <algorithm>
#include <set>

#include "indicator/fhir_id.hpp"
#include "indicator/normalize.hpp"

namespace mirobody {
namespace indicator {

namespace {

// v1 base scores by match path. Exact normalized-surface hit is the only path
// for now; alias/fuzzy land lower in later milestones. Kept in [0,1] so the
// ported cosine-era bonus/demote magnitudes can be layered on after
// recalibration (see plan).
const double kScoreExact = 1.0;

bool system_allowed(int sys, const std::vector<std::string>& systems) {
    if (systems.empty()) return true;
    const char* name = system_name(sys);
    for (size_t i = 0; i < systems.size(); ++i)
        if (systems[i] == name) return true;
    return false;
}

}  // namespace

std::vector<ResolveResult> Resolver::resolve(
    const std::string& term, int top_k,
    const std::vector<std::string>& systems) const {
    std::vector<ResolveResult> out;
    std::string key = normalize(term);
    if (key.empty()) return out;

    const std::vector<uint32_t>* hits = lex_.lookup(key);
    if (!hits) return out;

    std::set<uint32_t> seen;
    for (size_t i = 0; i < hits->size(); ++i) {
        const CodeMeta& m = lex_.code((*hits)[i]);
        int sys = m.system;
        if (!system_allowed(sys, systems)) continue;
        if (!seen.insert((*hits)[i]).second) continue;
        ResolveResult r;
        r.system = system_name(sys);
        r.code = m.code;
        r.name = m.name;
        r.score = kScoreExact;
        out.push_back(r);
    }

    // Stable rank: score desc, then code asc for determinism.
    std::stable_sort(out.begin(), out.end(),
                     [](const ResolveResult& a, const ResolveResult& b) {
                         if (a.score != b.score) return a.score > b.score;
                         return a.code < b.code;
                     });
    if (top_k > 0 && static_cast<int>(out.size()) > top_k)
        out.resize(static_cast<size_t>(top_k));
    return out;
}

}  // namespace indicator
}  // namespace mirobody
