# src/indicator — lexical terminology resolver

Maps free-text medical terms (Chinese drug / organism / virus names, lab
indicators) → standard codes (LOINC / SNOMED_CT / RXNORM / CVX / DCM).
**Pure C++, lexical-first, no embedding API** — the deterministic replacement
for the Python `mirobody.indicator` embedding-cosine resolver (which plateaued
at ~60–70% accuracy with a ~10% *deterministic* error rate on long-tail proper
nouns). See the design rationale in the plan.

A lexical miss is *empty* (recoverable, fixable by adding a synonym/rule and
locking it with a golden test) rather than embedding's *confident-wrong*.

## Pipeline (composable)

```
query → normalize → match (recall) → rank → ResolveResult[]
```

| Stage | File | Status |
| ----- | ---- | ------ |
| normalize (NFKC-lite + casefold + ws) | `normalize.*` | ✅ |
| lexicon artifact format + loader | `lexicon.*` | ✅ |
| match: **exact** normalized-surface | `resolve.*` | ✅ M1 |
| match: alias / substring / Aho-Corasick span / fuzzy | `resolve.*` | ⏳ M2 |
| rank: specificity 17-mask, common_test_rank, specimen tier, gates | `rank.*` | ⏳ M3 |
| coverage: UMLS all-SAB + ChEBI / MONDO / NCBI taxdump / LPSN / PubChem | emitter | ✅ M4 |

## Module layout

```
src/indicator/
  normalize.*   surface normalization (NFKC-lite / casefold / whitespace)
  word.*        word_tokens() tokenizer + build_word_vocab() (`words`) +
                build_synonyms() word-pair miner (`synonyms`)
  fhir_id.hpp   System enum + legacy (system<<60)|code DB-bigint helper
  lexicon.*     artifact format (LexiconBuilder writer / Lexicon reader) + dumps
  sources.*     ALL reference-data ingestion — readers, parsers, glob path
                resolver; exposes build_lexicon(b, opt) and build_units(...)
  resolve.*     runtime Resolver (query → ranked ResolveResult)
cli/indicator.cpp   thin: arg-parse + dispatch to the above
```

A code's identity is **(system, code string)** — `system` is a plain `uint8` enum
(`fhir_id.hpp`, up to 255 systems), `code` the vocabulary's own string. This is
the only stable, externally-meaningful identifier; the internal postings index is
artifact-local and not stable across rebuilds. `code_to_fhir_id` (the legacy
`(system<<60)|code` bigint packing) is kept **only** as a DB-compat helper for the
six FHIR-canonical systems — never round-trips to Python data.

## Artifact (`res/indicator/fhir_lexicon.bin`)

One little-endian binary, built by `cli/indicator build-lexicon`, consumed by
the runtime `Lexicon` loader. Format owned by `lexicon.hpp` so writer/reader
never drift: `LXC1` payload = header → code metadata → surfaces (sorted) →
postings → string blob. Per-code metadata reserves `rank_tier` + a 17-bit
`spec_mask` so the runtime runs zero regex (precomputed at build time in M3).
**Full byte-level spec:** [res/indicator/README.md](../../res/indicator/README.md).

**Compression.** The payload is zlib-wrapped (`LXCZ | u64 raw_len | deflate`),
decompressed on load. ~4.7× (528 MB → ~112 MB). The artifact is **not**
committed (gitignored under `res/indicator/`) — rebuilt from the reference tree. Deeper
size wins (FST + input trimming, mmap-able, on-device) are future work.

## Data sources

Two tiers — what's already pulled in, and the external sources to add next.

### Tier A — already ingested (UMLS MRCONSO all-SAB + LOINC + CVX)

The **English** coverage for most categories is already here (surfaces filtered to
ENG + CHI, everything bridged by CUI):

| Category | Already covered by |
|---|---|
| bacteria / virus / microorganism | **NCBI Taxonomy (~1.1M in UMLS)** · SNOMED organism · MeSH |
| drugs | RxNorm · NDDF · MMSL · MTHSPL · MeSH · NCI |
| chemicals | MeSH · NCI |
| disease | SNOMED disorder · **ICD-10-CM** · MeSH · OMIM |
| food | MeSH (partial) · LOINC food-IgE |
| abbreviations / lay variants | **CHV (~146k)** · per-source `SY` synonyms · our `abbrev.tsv` |

**Chinese ≈ LOINC-zh only (~92k, LNC-ZH-CN).** UMLS Chinese for drugs / organisms /
disease / chemicals / food is effectively empty — the crosscutting gap, handled
separately (NHSA, ICD-10-cn, curated).

### Tier B — external sources to add (English deepeners; not in UMLS, not in the reference tree)

Prefer permissively-licensed sources; license is a hard gate for a commercial
health app.

| Source | Category | Adds | License | Format |
|---|---|---|---|---|
| **ChEBI** | chemicals | chemical entities + synonyms, formula, CAS/KEGG xrefs | CC BY 4.0 ✅ | OBO / OWL / TSV |
| **PubChem** (CID-Synonym) | chemicals | very large trade/systematic synonym set | public domain ✅ | TSV.gz (large) |
| **FDA UNII / GSRS** | substances | UNII + substance synonyms | public ✅ | TSV |
| **FoodOn** | food | food items + parts + synonyms | CC BY 3.0 ✅ | OBO / OWL |
| **USDA FoodData Central** | food | food names / descriptions | public ✅ | CSV / JSON |
| **NCBI taxdump** (`names.dmp`) | organisms | full synonyms / common names beyond the UMLS subset | public ✅ | dmp |
| **LPSN** | bacteria | prokaryote nomenclature, basonyms / synonyms | free — verify redistribution ✅ | CSV / API |
| **ICTV** (VMR / MSL) | viruses | virus taxonomy names | free ✅ | xlsx |
| **MONDO** | disease | unified disease + rich synonyms + xrefs (SNOMED/ICD/OMIM/Orphanet) | CC BY 4.0 ✅ | OBO / OWL |
| DrugBank | drugs | brand / synonym depth | ⚠️ **commercial license** | XML |
| WHO ATC (bulk) | drugs | ATC classification names | ⚠️ licensed (use via RxNorm mapping) | — |

### Integration model (resolved)

Code identity is now `(system:uint8, code string)` — the artifact stores a plain
`uint8` system (up to 255), so each external source gets its own system enum
(`SYS_CHEBI`, `SYS_NCBITAXON`, `SYS_MONDO`, `SYS_FOODON`, `SYS_ICTV`, `SYS_LPSN`,
`SYS_PUBCHEM`). No bit-packing / DB-bigint constraint. Ingestion strategy per source:

1. **Anchor to an existing concept via cross-refs** where possible (MONDO→UMLS
   `xref`/SNOMED/ICD, organisms→NCBI taxid/name→SNOMED/CUI, chemicals→CAS/InChIKey/name)
   and attach the source's synonyms to that concept's code. *(Most of these are
   synonym sources for concepts we already have.)*
2. **Otherwise mint a new code** under the source's own system — a genuinely new
   concept (e.g. a ChEBI-only chemical) keyed by its native id (`CHEBI:100`).

## Build the lexicon (dev / offline)

Needs raw reference data under one local root, passed as `--ref`. The commands below
spell it `<ref>` — it lives outside the repo, so put it wherever you like and
substitute. Current sources: LOINC 2.82 core +
zhCN linguistic variant, CVX (en + `cvx_cn.csv`), **UMLS MRCONSO — all sources**
(SNOMED / LOINC / RxNorm native codes; MeSH / NCI / DrugBank / CHV / … via the
CUI-sharing bridge, with a `UMLS_CUI` fallback code so chemicals lacking a native
code aren't dropped; surfaces filtered to **ENG + CHI**), and a concatenated
foreign-alias TSV (`--aliases`, colloquial → English canonical → code).

Paths under `--ref` are **globbed, not hardcoded** — `sources.cpp::resolve_ref`
finds the newest `Loinc_*` / `umls-*` dir and the `zhCN*LinguisticVariant.csv` /
`lpsn_gss_*.csv` files, so new releases need no code edits.

Phases chain via files (each independently re-runnable):

```sh
# 1. build the artifact (+ optional full-vocabulary dump)
indicator build-lexicon --ref <ref> [--no-pubchem] --out res/indicator/fhir_lexicon.bin [--dump build/vocab_all.tsv]
# 2. word-token vocab from the dump (iterate the tokenizer here — ~22s, no rebuild)
indicator words --in build/vocab_all.tsv --out build/vocab_name.tsv
# word-level synonyms: mine token pairs from surfaces sharing a (system,code)
# (spelling/abbrev/plural + cross-lingual glucose↔葡萄糖) — ~60s, no rebuild.
# --lex merges authoritative single-token spelling variants from the SPECIALIST
# Lexicon (<ref>/LEX/LRSPL). Output is word_a·word_b·support·source
# (source = corpus | lex | corpus+lex). LRABR (abbrev→phrase, highly ambiguous)
# and LRAGR (inflection, redundant with the corpus) are intentionally not used.
indicator synonyms --in build/vocab_all.tsv --out build/synonyms.tsv --min-support 10 --lex <ref>/LEX
# unit thesaurus (LOINC Parts) + abbreviation table
indicator build-units --ref <ref> --out build/units.tsv --abbrev build/abbrev.tsv
# query
indicator resolve --lexicon res/indicator/fhir_lexicon.bin --top-k 5 "烟曲霉" "aspirin" "50-78-2"
```

Current coverage: **~5.9M codes / ~14.0M surfaces**, ~262 MB. Built from UMLS
all-SAB + LOINC + CVX + aliases, then the external deepeners: ChEBI (205k
chemicals), MONDO (56k diseases, xref-anchored to UMLS/SNOMED), NCBI taxdump
(2.85M organisms), LPSN (34k bacteria), and PubChem (enrich-only — 198k chemical
concepts gained CAS numbers + trade/systematic synonyms; **resolve by CAS works**,
e.g. `50-78-2` → aspirin). English chemical / disease / organism coverage is now
deep. Remaining gaps are all **Chinese-specific** (UMLS/external Chinese is sparse):
Chinese drug trade names (`阿托伐他汀` → NHSA `medicine_data.json`), colloquial
(`丙肝抗体`, `新型冠状病毒` → curated), Chinese chemical names (`高香草酸` → curated /
Chinese chemical source). These are the next data step, not more English ingest.

## Golden harness (accuracy gate)

`tests/indicator/golden.tsv` holds `<term> <TAB> <expected_code>` cases.
`tests/indicator/resolve_test.cpp` reports **recall** (analyte recognized —
isolates the recall stage) and **top-1** (right code ranked first — isolates
ranking), and CHECKs ratchet floors. Skips unless both env vars are set
(artifact + golden are not committed):

```sh
INDICATOR_LEXICON=res/indicator/fhir_lexicon.bin INDICATOR_GOLDEN=tests/indicator/golden.tsv \
  build/tests/mirobody_tests.exe "[golden]"
```

### Baseline (M1, 21 cases)

`recall 67% · top-1 10%`. Recall misses are colloquial/abbreviation/reordered
forms (→ M2 alias/curated); top-1 misses are correct-analyte-wrong-variant
(→ M3 rerank: e.g. `葡萄糖` matches Glucose but ranks "Mixed venous blood" over
the canonical Blood/Ser-Plas). Each number is a ratchet raised per milestone.
