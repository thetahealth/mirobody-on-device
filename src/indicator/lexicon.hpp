#pragma once

// The lexicon artifact: surface form -> standard code(s), plus per-code
// metadata for ranking. One file, produced by `cli/indicator build-lexicon`
// and consumed by the runtime Resolver.
//
// This header owns the on-disk format so the writer (emitter) and reader
// (runtime) can never drift: the emitter fills a LexiconBuilder and calls
// write(); the runtime calls Lexicon::load(). Format is little-endian (every
// target — Windows x64/ARM64, Android ARM, iOS — is LE).
//
//   magic "LXC1" | version | n_codes | n_surfaces | n_postings | blob_len
//   codes[n_codes]      : { u64 fhir_id, u32 code_off, u32 name_off,
//                           u32 rank_tier, u32 spec_mask }
//   surfaces[n_surfaces]: { u32 surface_off, u32 post_off, u32 post_count }
//                          (sorted by surface string, for future mmap binary
//                           search; the v1 loader builds a hash map instead)
//   postings[n_postings]: u32 code_index
//   blob[blob_len]      : NUL-terminated UTF-8 strings; offset 0 == ""
//
// Surface keys are already run through indicator::normalize() on BOTH sides.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mirobody {
namespace indicator {

// One match from resolve(): free text -> standard code.
struct ResolveResult {
    std::string system;  // "LOINC", "SNOMED_CT", ...
    std::string code;    // e.g. "2345-7"
    std::string name;    // display name
    double score;        // higher = better

    ResolveResult() : score(0.0) {}
};

// Per-code metadata held in memory after load. A code's stable identity is
// (system, code) — the internal code-index used in postings is artifact-local
// and NOT stable across rebuilds, so nothing outside the artifact references it.
struct CodeMeta {
    uint8_t system;      // System enum (see fhir_id.hpp)
    std::string code;    // original code string (LOINC keeps its dash, "CHEBI:100", …)
    std::string name;    // display name
    uint32_t rank_tier;  // common_test_rank bonus tier (0 = none)  [reserved]
    uint32_t spec_mask;  // 17-bit specificity name-mask           [reserved]

    CodeMeta() : system(0), rank_tier(0), spec_mask(0) {}
};

// ─── Reader ──────────────────────────────────────────────────────────

class Lexicon {
public:
    // Load an artifact. Returns false and fills `err` on any error.
    static bool load(const std::string& path, Lexicon& out, std::string& err);

    // Exact lookup of an already-normalized surface. Returns the matching
    // code indices, or nullptr if the surface is absent.
    const std::vector<uint32_t>* lookup(const std::string& normalized) const;

    const CodeMeta& code(uint32_t idx) const { return codes_[idx]; }
    size_t code_count() const { return codes_.size(); }
    size_t surface_count() const { return surfaces_.size(); }

private:
    std::vector<CodeMeta> codes_;
    std::unordered_map<std::string, std::vector<uint32_t> > surfaces_;
};

// ─── Writer ──────────────────────────────────────────────────────────

class LexiconBuilder {
public:
    LexiconBuilder() { blob_.push_back('\0'); }  // offset 0 == ""

    // Register a code; returns its dense index (idempotent per (system, code)).
    uint32_t add_code(uint8_t system, const std::string& code,
                      const std::string& name, uint32_t rank_tier = 0,
                      uint32_t spec_mask = 0);

    // Map a normalized surface form to a previously-added code index.
    void add_surface(const std::string& normalized, uint32_t code_idx);

    // Look up the code indices already mapped to a normalized surface, or null.
    // Used by alias ingestion to resolve a foreign alias's English target to the
    // codes that target already points at.
    const std::vector<uint32_t>* lookup(const std::string& normalized) const;

    // Find a code already registered as (system, code); returns true + its index.
    // Used by external ingestion to anchor via cross-reference (e.g. MONDO's
    // `xref: UMLS:Cxxxx` -> the existing UMLS_CUI code).
    bool find_code(uint8_t system, const std::string& code, uint32_t& out) const;

    bool write(const std::string& path, std::string& err) const;

    // Debug: dump every (surface -> code) row as TSV, sorted by surface, with
    // header `surface<TAB>system<TAB>code<TAB>name`. For manual review of what
    // the lexicon actually contains. Large (one row per posting).
    bool write_tsv(const std::string& path, std::string& err) const;

    // Debug: dump one row PER CODE, with all its synonym surfaces joined by
    // " | ", sorted by (system, name). Header `system<TAB>code<TAB>name<TAB>surfaces`.
    // Far more reviewable for synonym checking than the flat per-posting dump.
    bool write_tsv_by_code(const std::string& path, std::string& err) const;

    size_t code_count() const { return codes_.size(); }
    size_t surface_count() const { return surfaces_.size(); }

private:
    struct BCode {
        uint8_t system;
        uint32_t code_off;
        uint32_t name_off;
        uint32_t rank_tier;
        uint32_t spec_mask;
    };

    uint32_t intern(const std::string& s);

    std::vector<BCode> codes_;
    std::unordered_map<std::string, uint32_t> code_index_;  // "sys\tcode" -> idx
    std::unordered_map<std::string, std::vector<uint32_t> > surfaces_;
    std::unordered_map<std::string, uint32_t> intern_;      // string -> blob off
    std::vector<char> blob_;
};

}  // namespace indicator
}  // namespace mirobody
