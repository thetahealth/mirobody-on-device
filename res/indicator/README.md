# res/indicator — terminology artifacts

Runtime artifact(s) for the `src/indicator` terminology resolver. Currently just
`fhir_lexicon.bin` (gitignored, ~112 MB, rebuilt from the reference tree — see
[`src/indicator/README.md`](../../src/indicator/README.md)). The rest of this
file is its byte-level format spec.

## Runtime access (C++)

Most callers go through `indicator::Resolver` (`src/indicator/resolve.hpp`),
which normalizes the query, looks it up, and ranks. To read the artifact
directly, use `indicator::Lexicon` (`src/indicator/lexicon.hpp`):

```cpp
#include "indicator/lexicon.hpp"
#include "indicator/normalize.hpp"
using namespace mirobody::indicator;

Lexicon lex;
std::string err;
if (!Lexicon::load("res/indicator/fhir_lexicon.bin", lex, err)) { /* err */ }

// lookup() takes an ALREADY-normalized surface and returns the matching code
// indices (or null). Both sides must use the same normalize(), so the build-time
// emitter and this call agree on the key.
const std::vector<uint32_t>* hits = lex.lookup(normalize(u8"血小板"));
if (hits) {
    for (size_t i = 0; i < hits->size(); ++i) {
        const CodeMeta& m = lex.code((*hits)[i]);   // index into the Code section
        // m.fhir_id, m.code ("777-3"), m.name, m.rank_tier, m.spec_mask
    }
}
```

| Method | Returns | Notes |
|--------|---------|-------|
| `Lexicon::load(path, out, err)` | `bool` | reads + inflates the file, builds the in-memory index |
| `lookup(normalized)` | `const std::vector<uint32_t>*` | code indices for an exact normalized surface, or `null` |
| `code(idx)` | `const CodeMeta&` | per-code metadata for a `code_index` from `lookup` |
| `code_count()` / `surface_count()` | `size_t` | sizes |

> **Caller contract:** `lookup()` does **no** normalization — pass the result of
> `indicator::normalize()`. The v1 loader builds a hash map (`unordered_map`)
> from the Surface section; a future mmap reader can binary-search the
> sorted-on-disk surfaces instead, with the same `lookup()` signature.
>
> `LexiconBuilder::lookup()` (build side) has the same shape but queries
> surfaces added so far — used by alias ingestion to resolve a foreign alias's
> English target to the codes it already points at.

## `fhir_lexicon.bin` — on-disk format

Maps normalized surface forms → standard codes (LOINC / SNOMED_CT / RXNORM / CVX
/ DCM / THETA) plus per-code metadata for ranking.

- **Producer:** `cli/indicator build-lexicon` → `LexiconBuilder::write()`
- **Consumer:** `Lexicon::load()` (runtime resolver)
- **Source of truth:** [`src/indicator/lexicon.cpp`](../../src/indicator/lexicon.cpp)
  / [`lexicon.hpp`](../../src/indicator/lexicon.hpp). If this doc and the code
  ever disagree, the code wins — keep them in sync.

All integers are **little-endian**, unsigned. Offsets are **byte offsets into
the string blob**. The artifact is **not committed** (gitignored here, ~112 MB);
rebuild from the reference tree.

## Outer container

The file is one of two forms, distinguished by the first 4 bytes:

| Magic | Meaning |
|-------|---------|
| `LXCZ` | zlib-compressed wrapper (the default that `write()` emits) |
| `LXC1` | raw, uncompressed payload (also accepted by the loader) |

**`LXCZ` wrapper:**

```
offset  size  field
0       4     magic = "LXCZ"
4       8     u64   raw_len      # size of the inflated LXC1 payload
12      ...   deflate stream     # zlib (compress2 level 6) of the LXC1 payload
```

`load()` inflates bytes `[12 .. EOF)` into a `raw_len`-byte buffer, then parses
it as the `LXC1` payload below. (`raw_len < 4 GB` — single-shot zlib.)

## `LXC1` payload

Five sections, in order. Section offsets are computed from the header counts;
there is no per-section offset table.

### 1. Header (28 bytes)

```
offset  size  field
0       4     magic = "LXC1"
4       4     u32   version = 2
8       4     u32   n_codes
12      4     u32   n_surfaces
16      4     u32   n_postings
20      8     u64   blob_len
```

### 2. Code section — `n_codes × 17 bytes`

One record per distinct code, in dense index order (`code_index` 0..n_codes-1).
A code's identity is **(system, code string)**.

```
size  field
1     u8    system        # System enum (see table below)
4     u32   code_off      # blob offset of the original code string ("2345-7", "CHEBI:100")
4     u32   name_off      # blob offset of the display name
4     u32   rank_tier     # common_test_rank bonus tier (0 = none)   [reserved, M3]
4     u32   spec_mask     # 17-bit specificity name-mask             [reserved, M3]
```

(v1 stored a `u64` packed `fhir_id` here instead of `u8 system`; v2 stores the
system directly so there is no bit-budget on the number of systems.)

### 3. Surface section — `n_surfaces × 12 bytes`

One record per distinct normalized surface form, **sorted ascending by the
surface string** (so a future mmap reader can binary-search; the current loader
builds a hash map instead).

```
size  field
4     u32   surface_off   # blob offset of the normalized surface string
4     u32   post_start    # index into the postings array (in entries, not bytes)
4     u32   post_count    # number of postings for this surface
```

### 4. Postings — `n_postings × 4 bytes`

Flat `u32` array of `code_index` values. The codes for surface *i* are
`postings[post_start .. post_start + post_count)`. Each `code_index` indexes the
Code section (§2).

### 5. String blob — `blob_len bytes`

Concatenated **NUL-terminated UTF-8** strings. A `*_off` field is the byte offset
of the first char; the string runs to the next `\0`. **Offset 0 is the empty
string** (the blob always begins with a `\0`). Strings are interned (deduped), so
many `*_off` fields may point at the same bytes.

## System enum

A code's identity is `(system, code string)`. `system` is a `u8` (stored in the
code record), so up to 255 systems — **append-only, never reorder**. Indices 0–5
keep the Python `fhir/common.py` order only so the DB-compat helper can emit a
legacy packed bigint for those six on demand.

| enum | system | | enum | system |
|------|--------|---|------|--------|
| 0 | SNOMED_CT | | 7 | CHEBI |
| 1 | LOINC | | 8 | NCBITAXON |
| 2 | RXNORM | | 9 | MONDO |
| 3 | CVX | | 10 | FOODON |
| 4 | DCM | | 11 | ICTV |
| 5 | THETA | | 12 | LPSN |
| 6 | UMLS_CUI | | 13 | PUBCHEM |

The `code` string is stored verbatim (`code_off`) — e.g. `2345-7`, `73211009`,
`CHEBI:100`, `C0000005`, an NCBI taxid. `UMLS_CUI` (6) is the fallback for UMLS
concepts with no native SNOMED/LOINC/RxNorm/CVX code (code = the CUI).

### `code_to_fhir_id` (legacy DB helper only)

`fhir_id.hpp` still offers the Python `(system<<60)|code_int` bigint packing
(3-bit system + 60-bit code, LOINC dash stripped), but **only** as a helper for
callers that must write the six FHIR-canonical systems to a Postgres `bigint`
column. It is **not** the artifact's identity and is never round-tripped to
Python data.

## Parse sketch

```
buf = read(file)
if buf[0:4] == "LXCZ": buf = zlib_inflate(buf[12:], raw_len = u64(buf[4:12]))
assert buf[0:4] == "LXC1" and version == 2
n_codes, n_surf, n_post = u32(buf[8:]), u32(buf[12:]), u32(buf[16:])
blob_len = u64(buf[20:])
codes  = buf[28 :]                    # n_codes * 17  (u8 system + u32*4)
surf   = codes + n_codes*17           # n_surf  * 12
post   = surf  + n_surf*12            # n_post  * 4
blob   = post  + n_post*4             # blob_len
str(off) = C-string at blob+off
```
