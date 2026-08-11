#include "indicator/resolve.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>

#include "indicator/fhir_id.hpp"
#include "indicator/normalize.hpp"

namespace mirobody {
namespace indicator {

namespace {

// Base scores by match path. An exact normalized-surface hit is the whole
// query; a span is one field of it, so it stays strictly below and is ordered
// by how much of the query it accounts for. Kept in [0,1] so the ported
// cosine-era bonus/demote magnitudes can be layered on after recalibration.
const double kScoreExact = 1.0;
const double kScoreSpan = 0.5;      // one field of the query, not the whole of it
// Sorts behind every ranked term without overflowing when compared.
const uint32_t kUnranked = 0xFFFFFFFFu;

bool system_allowed(int sys, const std::vector<std::string>& systems) {
    if (systems.empty()) return true;
    const char* name = system_name(sys);
    for (size_t i = 0; i < systems.size(); ++i)
        if (systems[i] == name) return true;
    return false;
}

// Structural separators only: what a report puts BETWEEN a panel and the
// analyte, never inside an analyte's own name. `血生化_电解质_钾` is three
// fields; `hs-CRP`, `NT-proBNP`, `β-羟丁酸` and `platelet count` are one each,
// which is why '-' and ' ' are not in here. Written as UTF-8 byte sequences
// since the normalized form is UTF-8 and the multi-byte ones are all CJK
// punctuation a Chinese report actually uses.
const char* kSeparators[] = {
    "_", "/", "\\", "|", ",", ";", ":", "(", ")", "[", "]",
    "·", "、", "，", "；", "：", "（", "）", "【", "】", "－", 0
};

// The query cut at those separators, longest field first, empties dropped. Only
// whole fields are tried: a substring that starts mid-name is how a lexical
// matcher starts answering `钾` with `钾盐`, and this stage exists to widen
// recall, not to invent it.
std::vector<std::string> split_fields(const std::string& key) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i < key.size();) {
        size_t hit = 0;
        for (int s = 0; kSeparators[s]; ++s) {
            size_t n = std::strlen(kSeparators[s]);
            if (key.compare(i, n, kSeparators[s]) == 0) { hit = n; break; }
        }
        if (hit) {
            if (i > start) parts.push_back(key.substr(start, i - start));
            i += hit;
            start = i;
        } else {
            ++i;
        }
    }
    if (start < key.size()) parts.push_back(key.substr(start));
    return parts;
}

// A rule tried here and REVERTED, kept so it is not tried again blind:
// preferring a term whose own name begins with the matched surface. It fixes
// 786-4 (MCHC, COMPONENT Hemoglobin, rank 13) losing to nothing while 718-7
// (rank 17) is what `Hemoglobin` means -- and it cost 13 points of top-1 on the
// ranking group, 82% -> 69%. Two reasons, both obvious afterwards: against an
// abbreviation it promotes whatever name happens to start with those two or
// three letters over the right term (`TG` over Thyroglobulin, rank 729), and
// against a Chinese query it is inert, since every LOINC name is English. The
// distinction it was reaching for is really SYSTEM -- 718-7 is Bld, 786-4 is
// RBC -- which is a build-time property and belongs in spec_mask with the rest
// of M3b.

}  // namespace

std::vector<ResolveResult> Resolver::resolve(
    const std::string& term, int top_k,
    const std::vector<std::string>& systems) const {
    std::vector<ResolveResult> out;
    std::string key = normalize(term);
    if (key.empty()) return out;

    std::set<uint32_t> seen;
    std::vector<uint32_t> ranks;   // parallel to `out`
    std::vector<uint32_t> tiers;   // parallel to `out`
    std::vector<uint32_t> offrep;  // parallel to `out`
    std::vector<uint32_t> qual;    // parallel to `out`
    struct Hit { const std::vector<uint32_t>* codes; double score; std::string surface; };
    std::vector<Hit> found;

    const std::vector<uint32_t>* exact = lex_.lookup(key);
    if (exact) {
        Hit h;
        h.codes = exact;
        h.score = kScoreExact;
        h.surface = key;
        found.push_back(h);
    } else {
        // Span fallback, by ROLE rather than by size. A report writes the panel
        // in front of the analyte -- 血生化_电解质_钾, 尿液_尿液分析_蛋白质 --
        // so the rightmost field that resolves is the analyte and everything to
        // its left is context. Taking the longest field instead answered
        // `营养_维生素、矿物质_钙` with SNOMED's "Mineral (substance)", because
        // 矿物质 is longer than 钙 and a coverage score made it win.
        //
        // Context fields are not candidates. They are worth reading -- 血生化
        // says CHEM, 尿液 says the specimen -- but as a constraint on the
        // analyte's candidates, never as candidates of their own.
        //
        // Only reached when the whole query missed, so anything that resolves
        // exactly keeps exactly the candidates it had.
        std::vector<std::string> fields = split_fields(key);
        for (size_t i = fields.size(); i-- > 0;) {
            const std::vector<uint32_t>* codes = lex_.lookup(fields[i]);
            if (!codes) continue;
            Hit h;
            h.codes = codes;
            h.score = kScoreSpan;
            h.surface = fields[i];
            found.push_back(h);
            break;   // the analyte is found; the rest is context
        }
    }

    // Best score wins per code: a field that is both long and known should not
    // be pushed down by the same code turning up under a shorter one.
    for (size_t f = 0; f < found.size(); ++f) {
        const std::vector<uint32_t>& hits = *found[f].codes;
        for (size_t i = 0; i < hits.size(); ++i) {
            const CodeMeta& m = lex_.code(hits[i]);
            int sys = m.system;
            if (!system_allowed(sys, systems)) continue;
            if (!seen.insert(hits[i]).second) continue;
            ResolveResult r;
            r.system = system_name(sys);
            r.code = m.code;
            r.name = m.name;
            r.score = found[f].score;
            ranks.push_back(m.rank_tier ? m.rank_tier : kUnranked);
            tiers.push_back(m.spec_mask & 7u);        // bits 0-2: specimen tier
            offrep.push_back((m.spec_mask >> 3) & 1u); // bit 3: not report content
            qual.push_back((m.spec_mask >> 4) & 1u);   // bit 4: qualified COMPONENT
            out.push_back(r);
        }
    }

    // Rank: match quality first, then LOINC's own ordering among the terms that
    // survived it. A name is shared by many terms -- `Glucose` by hundreds --
    // and COMMON_TEST_RANK is the release's own statement of which one is
    // actually ordered, so it decides where exact match cannot. Unranked terms
    // (every non-LOINC system, and the 89,347 LOINC terms LOINC does not rank)
    // keep the old code-ascending order behind them, which is arbitrary but
    // deterministic.
    std::vector<size_t> order(out.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) {
                         if (out[a].score != out[b].score) return out[a].score > out[b].score;
                         // A questionnaire item is not an answer to anything a
                         // report prints, however it scores -- category first.
                         if (offrep[a] != offrep[b]) return offrep[a] < offrep[b];
                         // Specimen before rank: a derived fraction is the wrong
                         // answer to a bare analyte name however commonly it is
                         // ordered -- MCHC (RBC, rank 13) vs Hemoglobin (Bld, 17).
                         if (tiers[a] != tiers[b]) return tiers[a] < tiers[b];
                         // A name that does not say `free` does not mean the free
                         // fraction, however often that one is ordered.
                         if (qual[a] != qual[b]) return qual[a] < qual[b];
                         if (ranks[a] != ranks[b]) return ranks[a] < ranks[b];
                         return out[a].code < out[b].code;
                     });
    std::vector<ResolveResult> sorted;
    sorted.reserve(out.size());
    for (size_t i = 0; i < order.size(); ++i) sorted.push_back(out[order[i]]);
    out.swap(sorted);
    if (top_k > 0 && static_cast<int>(out.size()) > top_k)
        out.resize(static_cast<size_t>(top_k));
    return out;
}

}  // namespace indicator
}  // namespace mirobody
