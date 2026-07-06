#pragma once

// Reference-data ingestion for the lexicon + unit thesaurus. All the vocabulary
// parsers (LOINC / CVX / UMLS MRCONSO / aliases / ChEBI / MONDO / NCBI taxdump /
// LPSN / PubChem) and their readers live in sources.cpp; the CLI just parses
// args and calls these. Offline / dev-time (reads E:/ref, streams multi-GB
// files) — not used at runtime.

#include <string>

#include "indicator/lexicon.hpp"

namespace mirobody {
namespace indicator {

struct BuildOptions {
    std::string ref;      // reference-data root (e.g. E:/ref)
    std::string aliases;  // optional concatenated "src<TAB>dst" alias TSV
    std::string lpsn;     // optional LPSN csv path override (else globbed under ref)
    bool pubchem;         // include the (large) PubChem enrichment pass

    BuildOptions() : pubchem(true) {}
};

// Ingest all configured sources into `b` (fills codes + surfaces). Logs progress
// to stderr. The caller owns writing the artifact and any dumps.
void build_lexicon(LexiconBuilder& b, const BuildOptions& opt);

// Build the LOINC-Part unit thesaurus -> `out` TSV; if `abbrev_out` is non-empty
// also harvest the abbreviation table. Returns 0 on success.
int build_units(const std::string& ref, const std::string& out,
                const std::string& abbrev_out);

}  // namespace indicator
}  // namespace mirobody
