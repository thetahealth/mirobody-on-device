#pragma once

// (system, code) <-> packed bigint fhir_id, plus the code-system enum.
//
// C++ port of the packing logic in the Python `mirobody.indicator`
// package (fhir/common.py). The packed id is the postings key in the
// lexicon artifact, so the layout MUST stay bit-compatible with Python:
//
//   layout: [3-bit system | 60-bit code]  (63 bits, fits signed int64)
//   systems: append-only — the index is baked into every persisted id.
//
// `code_to_int` mirrors the Python rules: LOINC strips its dash, other
// numeric vocabs parse decimally. DCM / THETA use a one-way blake2b hash
// in Python; we do NOT reconstruct those here — the lexicon artifact
// carries the original code string per entry, so the runtime never needs
// int_to_code. `code_to_int` therefore rejects DCM/THETA (the emitter is
// responsible for hashing those, if/when it ingests them).

#include <cstdint>
#include <string>

namespace mirobody {
namespace indicator {

// Append-only; index is bit-packed into every fhir_id. Matches SYSTEMS in
// fhir/common.py.
// Code-system enum. In the lexicon artifact a code's identity is (system, code
// string) — `system` is stored as a plain u8 (so up to 255 systems; no bit
// budget). Indices 0..5 keep the Python SYSTEMS order (fhir/common.py) only so
// the DB-compat helper `code_to_fhir_id` below can still emit a legacy packed
// bigint for those six FHIR-canonical systems on demand. Everything past 5 is
// C++-only and never round-trips to Python. Append-only.
enum System {
    SYS_SNOMED_CT = 0,
    SYS_LOINC = 1,
    SYS_RXNORM = 2,
    SYS_CVX = 3,
    SYS_DCM = 4,
    SYS_THETA = 5,
    SYS_CUI = 6,       // UMLS concept fallback (code = CUI)
    SYS_CHEBI = 7,     // ChEBI chemical
    SYS_NCBITAXON = 8, // NCBI Taxonomy organism
    SYS_MONDO = 9,     // MONDO disease
    SYS_FOODON = 10,   // FoodOn food
    SYS_ICTV = 11,     // ICTV virus taxonomy
    SYS_LPSN = 12,     // LPSN prokaryote nomenclature
    SYS_PUBCHEM = 13,  // PubChem compound
    SYS_COUNT = 14
};

inline const char* system_name(int sys) {
    switch (sys) {
        case SYS_SNOMED_CT: return "SNOMED_CT";
        case SYS_LOINC: return "LOINC";
        case SYS_RXNORM: return "RXNORM";
        case SYS_CVX: return "CVX";
        case SYS_DCM: return "DCM";
        case SYS_THETA: return "THETA";
        case SYS_CUI: return "UMLS_CUI";
        case SYS_CHEBI: return "CHEBI";
        case SYS_NCBITAXON: return "NCBITAXON";
        case SYS_MONDO: return "MONDO";
        case SYS_FOODON: return "FOODON";
        case SYS_ICTV: return "ICTV";
        case SYS_LPSN: return "LPSN";
        case SYS_PUBCHEM: return "PUBCHEM";
        default: return "";
    }
}

// The canonical URL a FHIR `Coding.system` carries, or "" when this project has
// no business asserting one.
//
// The first five are published identifiers -- HL7 names them, and a receiving
// system will recognise them. The OBO ontologies use their own PURLs, which FHIR
// implementations follow by convention rather than by specification. The rest are
// left empty on purpose: a Coding with no system is weak but honest, while a
// Coding with an invented system URL is a claim about interoperability that
// nobody honoured. THETA is ours to define once the minting policy exists -- see
// fine-tuning/README.md on the 335 concepts no standard carries.
inline const char* system_url(int sys) {
    switch (sys) {
        case SYS_SNOMED_CT: return "http://snomed.info/sct";
        case SYS_LOINC: return "http://loinc.org";
        case SYS_RXNORM: return "http://www.nlm.nih.gov/research/umls/rxnorm";
        case SYS_CVX: return "http://hl7.org/fhir/sid/cvx";
        case SYS_DCM: return "http://dicom.nema.org/resources/ontology/DCM";
        case SYS_CHEBI: return "http://purl.obolibrary.org/obo/chebi.owl";
        case SYS_MONDO: return "http://purl.obolibrary.org/obo/mondo.owl";
        default: return "";
    }
}

// Inverse of system_name; returns -1 for an unknown name.
inline int system_from_name(const std::string& name) {
    for (int i = 0; i < SYS_COUNT; ++i)
        if (name == system_name(i)) return i;
    return -1;
}

const int kSysBits = 3;
const int kCodeBits = 60;
const uint64_t kCodeMask = (static_cast<uint64_t>(1) << kCodeBits) - 1;

// Parse a vocabulary code string to its 60-bit integer. Returns false when
// the code is non-numeric for a numeric vocab, exceeds the budget, or the
// system is hash-based (DCM/THETA) — those are not parseable here.
inline bool code_to_int(int sys, const std::string& code, uint64_t& out) {
    if (sys == SYS_DCM || sys == SYS_THETA) return false;
    std::string digits;
    digits.reserve(code.size());
    // UMLS CUI is "C" + 7-8 digits (C0000005); strip the leading 'C'.
    size_t begin = 0;
    if (sys == SYS_CUI && !code.empty() && (code[0] == 'C' || code[0] == 'c')) begin = 1;
    for (size_t i = begin; i < code.size(); ++i) {
        char c = code[i];
        if (c == '-' && sys == SYS_LOINC) continue;  // LOINC check digit dash
        if (c < '0' || c > '9') return false;
        digits.push_back(c);
    }
    if (digits.empty()) return false;
    uint64_t n = 0;
    for (size_t i = 0; i < digits.size(); ++i) {
        n = n * 10 + static_cast<uint64_t>(digits[i] - '0');
        if (n > kCodeMask) return false;  // overflow / budget
    }
    out = n;
    return true;
}

// Pack (system, code) -> fhir_id. Returns false if the code can't be parsed.
inline bool code_to_fhir_id(int sys, const std::string& code, uint64_t& out) {
    uint64_t n;
    if (!code_to_int(sys, code, n)) return false;
    out = (static_cast<uint64_t>(sys) << kCodeBits) | n;
    return true;
}

inline int fhir_id_system(uint64_t fhir_id) {
    return static_cast<int>(fhir_id >> kCodeBits);
}

}  // namespace indicator
}  // namespace mirobody
