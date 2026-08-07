# src/fhir — FHIR R4

A RESTful **FHIR R4** endpoint embedded in the mirobody core, plus the
terminology machinery that backs it. The end-to-end goal: an uploaded document
(lab report, discharge summary, …) is parsed into indicators and values, the
units are normalized, the indicators are mapped to **SNOMED CT / LOINC / RxNorm**
codes, and the results are materialized as FHIR resources served over this API.

This is the C++ port of the runtime half of the Python `mirobody.indicator`
package. The offline build tooling (siblings / bridge / merge / embeddings /
taxonomy — which ingest UMLS, SNOMED CT, LOINC, RxNorm and emit the artifact
files) stays in Python; the C++ core only *consumes* the artifacts it produces.

## Status

| Phase | Scope | State |
| ----- | ----- | ----- |
| 1 | **Unit normalization** ([units/](units/)) | ✅ done |
| 2 | **RESTful FHIR R4 server** ([rest.cpp](rest.cpp), [resource.cpp](resource.cpp), [write.cpp](write.cpp), [store.cpp](store.cpp)) | ✅ done |
| 3 | **Terminology resolve** — moved to [`src/indicator/`](../indicator/) and **redefined lexical-first** (deterministic match → rerank), replacing the embed→cosine design. The offline lexicon emitter is now C++ too (`cli/indicator build-lexicon`), not Python. | 🚧 in progress (M1) |
| 4 | **Document → indicators → FHIR** pipeline wiring | ⏳ planned |
| 5 | concept-graph expansion + taxonomy category view | ⏳ planned |

## Module layout

```
src/fhir/
  fhir.hpp / .cpp     # shared constants: supported versions/types, id + type validators
  resource.hpp / .cpp # structural validation of a generic resource (Health Connect rules)
  store.hpp / .cpp    # DB-backed CRUD over the fhir_resources table (per user)
  rest.hpp / .cpp     # FhirService: registers the FHIR routes on the Router
  units/              # free-text "value + unit" -> canonical UCUM + LOINC PROPERTY family
    families.*        # UCUM unit -> family table (+ ambiguous-unit sets)
    tokens.*          # multilingual morpheme + alias data (en/zh/ja/ko/ru/de/fr/es)
    normalize.*       # clean / tokenize-compose / normalize_unit / parse_value_unit
```

## Unit normalization (`units/`)

Pure local computation — no DB, no embedding API. Parses free-text "value + unit"
strings into a canonical UCUM unit and the LOINC PROPERTY family that
disambiguates LOINC candidates (mg/dL → `MCnc`, mmol/L → `SCnc`).

```cpp
#include "fhir/units/normalize.hpp"
#include "fhir/units/families.hpp"
using namespace mirobody::fhir::units;

normalize_unit("MG/DL");          // "mg/dL"
normalize_unit(u8"毫摩尔每升");    // "mmol/L"   (zh, tokenize-compose)
normalize_unit("mmHg");           // "mm[Hg]"

ParsedQuantity q = parse_value_unit("<5.6 mg/dL");
// q.comparator == "<", q.value == 5.6, q.unit == "mg/dL"

unit_family("mmol/L");            // "SCnc"
unit_families("%");               // {MFr, NFr, AFr, VFr, SFr, CFr, LenFr, RelACnc, RelRto}
```

Pipeline: NFKC-lite (full-width forms + superscripts) → symbol fold (µ/×/° …,
drop spaces) → flat alias lookup → annotation strip → greedy morpheme
tokenize-compose. Case is preserved (UCUM is case-sensitive). The data tables
are a verbatim copy of the Python `units/tokens.py` + `units/families.py`; keep
them in sync.

> **NFKC note.** The reference uses Python's full `unicodedata` NFKC. To stay
> dependency-free (no ICU), this port implements a targeted NFKC-lite covering
> the compatibility characters that occur in clinical unit strings (full-width
> ASCII U+FF01..FF5E, ideographic space, superscript digits). Exotic
> compatibility codepoints outside that set are not folded.

## RESTful FHIR R4 server

`FhirService` registers the routes on the shared `Router` (constructed in
[src/server/server.cpp](../server/server.cpp)). Resources are stored as **generic validated
JSON** — we don't hard-model each resource type. Routes are served under
`HTTP_URI_PREFIX` when set.

| Method | Path | Purpose |
| ------ | ---- | ------- |
| GET | `/fhir/metadata` | CapabilityStatement (public) |
| POST | `/fhir` | batch / transaction Bundle |
| GET | `/fhir/{type}` | search → searchset Bundle (`_id`, `_count`, `_offset`) |
| POST | `/fhir/{type}` | create (server-assigned id) → 201 |
| GET | `/fhir/{type}/{id}` | read → 200 / 404 / 410 (deleted) |
| PUT | `/fhir/{type}/{id}` | update or create-with-id → 200 / 201 |
| DELETE | `/fhir/{type}/{id}` | delete (idempotent) → 204 |

Bodies are `application/fhir+json`; errors come back as an `OperationOutcome`.
All resource routes require a bearer JWT (medical data) and are **scoped to the
authenticated user** — each user has an independent resource space, matching the
single-patient model.

### Validation (`resource.cpp`)

Mirrors the structural subset of the Android Health Connect medical-records
write rules:

- body must be a JSON object;
- `resourceType` present, a string, well-formed (`^[A-Z][A-Za-z0-9]*$`);
- `id`, when present, a valid FHIR id (`[A-Za-z0-9.-]{1,64}`);
- no `contained` resources;
- no top-level field may be JSON `null`.

The server injects `id` (on create) and `meta.versionId` / `meta.lastUpdated`
into the stored resource.

### Write bookkeeping (`write.cpp`)

`write_resource()` is the single place a validated resource becomes a row: it
assigns the id (a fresh uuid, or the caller's for an upsert), bumps `version_id`
off whatever is already stored, stamps `updated_at`, injects `id` + `meta`, and
upserts. Three paths share it and must agree, or the same resource read back
through a different door would look like a different resource:

| caller | id | semantics |
| --- | --- | --- |
| `POST /fhir/{type}` | server-assigned | create; a client id in the body is ignored |
| `PUT /fhir/{type}/{id}` | the URL's | upsert, version bumped |
| `mirobody_health_store` (C ABI) | the caller's, deterministic | on-device ingest; re-syncing a window replaces its readings instead of duplicating them |

Parsing and validation stay with the caller: the REST layer maps issues onto an
OperationOutcome with per-issue status codes, the C ABI collapses them into one
error string, and folding either policy in would force the other to unpick it.

### Persistence (`store.cpp`)

One portable table, declared per dialect in `res/sql/<dialect>/1_health.sql`:

```
fhir_resources(user_id, resource_type, resource_id, version_id,
               updated_at, deleted_at, content,
               PRIMARY KEY (user_id, resource_type, resource_id))
```

Works across the linked SQL backend (SQLite on mobile, PostgreSQL on desktop,
…). `updated_at` / `deleted_at` are unix-ms lifecycle stamps (the FHIR
`meta.lastUpdated` instant lives inside `content`); search orders by `updated_at`
and it backs the Last-Modified header. Delete is a soft delete (`deleted_at` set),
so a later read returns 410 Gone.

## Limitations (current skeleton)

- **Generic model only** — no deep per-field FHIR StructureDefinition checks
  (required fields, primitive regexes, complex-type shapes). Structural rules
  only, as listed above.
- **No `_history` / versioning reads** — `version_id` is tracked and surfaced in
  `meta` + `ETag`, but `GET .../_history` and `vread` are not implemented.
- **Batch, not atomic transaction** — `POST /fhir` runs entries independently;
  there is no rollback (the DB layer has no multi-statement transaction wrapper
  yet), so a `transaction` Bundle is processed with batch semantics.
- **Search is minimal** — `_id`, `_count`, `_offset` only; no general search
  parameters, `_include`, or chaining yet.
- Terminology resolve (Phase 3) and the document → FHIR pipeline (Phase 4) are
  not built yet.

## Tests

`tests/fhir/units_test.cpp` and `tests/fhir/resource_test.cpp` (Catch2). Run:

```cmd
build\tests\mirobody_tests.exe "[fhir]"
```

The REST handlers themselves are exercised end-to-end against a running server
with a database; the unit tests cover the units engine and the validator, which
have no DB dependency.
