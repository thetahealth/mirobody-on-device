-- Health-data projection layer: the query- and semantic-search-optimized read
-- model derived from `fhir_resources` (1_health.sql), which is the single source
-- of truth. Every row here is materialized on FHIR write and is fully derivable
-- from a fhir_resources row (via the source_id back-pointer), so the
-- whole layer can be rebuilt by replaying FHIR. This is the v2 successor to the
-- Python `th_series_dim` (dimension) + `th_series_data` (facts) split, minus the
-- terminology-mapping step: the (system, code) identity is already resolved at
-- write time by the lexical resolver (src/indicator), so this layer only stores.
-- User health data is coded LOINC-first, with SNOMED CT used only where LOINC
-- cannot express the observation -- a two-system space small enough that the
-- packed fhir_id keys every indicator (see health_indicators.id).
--
-- All three health sources -- device (Apple/Samsung/Health Connect, Fitbit/…),
-- EHR (SMART on FHIR), and parsed uploaded documents -- land in fhir_resources as
-- Observations and are projected here; `origin` records which one.
--
-- created_at / updated_at / effective_start / effective_end / deleted_at are unix
-- milliseconds that the application stamps at every write site
-- (platform::now_unix_ms), matching fhir_resources and memories -- no DB trigger, no tz.
--
-- This file also defines health_ingest_staging (below the projection tables) --
-- the transient inbox where raw high-frequency device uploads land before the
-- worker turns them into fhir_resources + health_facts.

-- health_indicators -- the indicator dimension, GLOBAL (concept metadata is
-- user-independent: "Fasting glucose" is the same for everyone, so its display
-- name / unit / embedding are shared and stored once). Successor to th_series_dim,
-- but keyed by the resolver's (system, code) rather than a free-text
-- original_indicator, and carrying the embedding used for semantic recall of the
-- indicator by meaning (NL query -> indicator).
--
-- Only lazily materialized for indicators that actually appear in some user's
-- data, so this stays in the low thousands of rows; a per-query in-process cosine
-- over that set is enough and no ANN index is needed (mirrors the `memories`
-- rationale). The embedding is stored verbatim as a little-endian float16 BYTEA
-- (1024 halves = 2 KB for the shipped embedders; widened to float32 before the
-- cosine). A pgvector `vector(1024)` + HNSW column is the natural upgrade if the
-- seen-indicator set ever grows past what an in-process scan can serve, and it
-- slots in behind the same lookup -- but float16 BYTEA is what keeps this schema
-- identical across the pg / mysql / sqlite backends.
CREATE TABLE IF NOT EXISTS health_indicators (
    -- id is the packed (system, code) fhir_id (indicator/fhir_id.hpp), computed
    -- by the app at projection time -- deterministic and stable across rebuilds,
    -- so facts stay linked (indicator_id = fhir_id) without a sequence lookup and
    -- each table can be re-derived from FHIR independently. This works only
    -- because user health data is scoped to LOINC (primary) + SNOMED CT (fallback
    -- when LOINC can't express it): both are system <= 1 (fit the 3-bit field)
    -- with numeric codes inside the 60-bit budget -- exactly code_to_fhir_id's
    -- domain. The readable system/code are kept alongside (this table is tiny) so
    -- display and FHIR Observation.code round-trips need no unpack / check-digit
    -- recompute.
    id          BIGINT       PRIMARY KEY,  -- packed (system<<60)|code fhir_id (app-computed)
    system      SMALLINT     NOT NULL,     -- indicator::System: 1=LOINC (primary), 0=SNOMED_CT (fallback)
    code        VARCHAR(64)  NOT NULL,     -- the vocabulary's own code string ("2345-7", SNOMED SCTID)
    display     VARCHAR(200) NOT NULL,     -- canonical display name (e.g. "空腹血糖 / Fasting glucose")
    category    VARCHAR(200),              -- grouping for UI/agent (血糖 / 血脂 / 生命体征 / …)
    unit        VARCHAR(64),               -- canonical unit (fhir/units normalized)
    embedding   BYTEA,                     -- little-endian float16 vector (1024 halves); NULL until embedded
    created_at  BIGINT       NOT NULL,     -- unix milliseconds (app-stamped)
    updated_at  BIGINT                     -- unix milliseconds; set on re-embed / metadata refresh, NULL until first
    -- No separate UNIQUE(system, code): id is a bijection of that pair, so
    -- PRIMARY KEY(id) already enforces one row per concept.
);

------------------------------------------------------------------------------

-- health_facts -- the flattened observations (numeric value + clinical time),
-- scoped per user. Successor to th_series_data. Carries no embedding of its own:
-- semantic recall hits health_indicators first, then reads the matched
-- indicator's facts here by (user_id, indicator_id[, effective_start]).
--
-- Each row points back to the fhir_resources row it was projected from
-- (source_id, an Observation) -- that is the sync/rebuild anchor: a FHIR write
-- upserts the fact(s), a FHIR delete finds them via idx_health_facts_source and
-- soft-deletes them.
--
-- effective_start / effective_end map FHIR's effectiveDateTime (an instant:
-- end == start) and effectivePeriod (start, end). The range itself carries the
-- span, so it needs no separate period/granularity enum: an aligned rollup
-- bucket (daily mean HR = [day start, day end]) and an irregular interval event
-- (a sleep session, a workout) are the same shape, and a point reading is just
-- end == start. A downsampled high-frequency stream is one fact per bucket.
--
-- Value is split into two nullable columns, exactly one set per fact: value_num
-- carries the numeric majority (labs / vitals / wearables) so trends and
-- thresholds run as plain SQL (AVG / MIN / MAX / value_num > x) -- the upgrade
-- over the Python layer, which stored everything as text; value_text carries
-- non-numeric results ("阳性", "Reactive", blood type, coded conclusions).
-- "value_num IS NOT NULL" is the numeric/qualitative discriminator, so no
-- separate value-type column is needed. Compound observations are split at
-- projection time into one fact per FHIR component (blood pressure 120/80 ->
-- two facts, systolic LOINC 8480-6 + diastolic 8462-4, each numeric); a value
-- with a comparator ("<5") stores 5 in value_num and the raw form in value_text.
-- Full fidelity always remains in fhir_resources (the source of truth).
CREATE TABLE IF NOT EXISTS health_facts (
    user_id       BIGINT           NOT NULL,
    indicator_id  BIGINT           NOT NULL,   -- -> health_indicators.id (the packed (system, code) fhir_id)
    value_num     DOUBLE PRECISION,            -- numeric reading (trends/charts); NULL for non-numeric
    value_text    TEXT,                        -- non-numeric result verbatim; NULL for pure numeric
    unit          VARCHAR(64),                 -- unit as stored on this reading (fhir/units normalized)
    effective_start BIGINT         NOT NULL,   -- observation's clinical time / bucket start, unix ms
    effective_end   BIGINT         NOT NULL,   -- bucket / interval end, unix ms; == effective_start for an instant
    origin        SMALLINT         NOT NULL,   -- health::timeseries::Origin: 1=device 2=ehr 3=document 4=manual
    source_id     VARCHAR(64)      NOT NULL,   -- back-pointer: fhir_resources.resource_id. No source_type
                                              -- column: only Observations project here, so the source's
                                              -- resource_type is always 'Observation' (join back with it).
    deleted_at    BIGINT,                      -- soft delete: NULL = live, unix ms = when deleted
    created_at    BIGINT           NOT NULL,   -- unix milliseconds (app-stamped)
    updated_at    BIGINT,                      -- unix milliseconds; set on update, NULL until first
    -- One fact per (user, indicator, start) per originating FHIR resource; a
    -- re-projection of the same resource upserts rather than duplicates. No
    -- surrogate id: nothing references a single fact, and this natural key IS
    -- the identity (mirrors fhir_resources' composite PK). Its leading prefix
    -- (user_id, indicator_id, effective_start) also serves the trend range scan,
    -- so no separate trend index is needed. effective_end is payload, not key:
    -- one indicator can't have two facts starting at the same instant from one
    -- source Observation.
    PRIMARY KEY (user_id, indicator_id, effective_start, source_id)
);

-- Reverse lookup used by the FHIR write/delete sync path: all facts projected
-- from a given source Observation.
CREATE INDEX IF NOT EXISTS idx_health_facts_source
    ON health_facts (source_id);


------------------------------------------------------------------------------

-- health_ingest_staging -- TRANSIENT ingest queue for raw high-frequency device
-- uploads. NOT part of the projection above, and NOT durable storage. A batch
-- lands here so the hot path can ack cheaply -- a single INSERT, no parsing, no
-- Parquet dependency -- then the DuckDB worker drains it: archives the raw
-- samples to object-storage Parquet (the durable raw store), writes period-
-- summary Observations to fhir_resources, projects facts into health_facts, and
-- DELETES the row. So rows are short-lived (bounded by processing lag) while the
-- table itself is permanent -- it is NOT a SQL TEMPORARY table, and NOT the
-- durable raw store (that is Parquet). Discrete data (labs, EHR, documents) is
-- already clean and is written straight to fhir_resources, bypassing this queue;
-- only the raw wearable stream lands here.
--
-- Successor to the Python series_data's landing role, but transient: its "keep
-- raw forever" duty moved to Parquet.
--
-- Drain model: the worker claims pending rows (all rows are pending -- a row is
-- deleted on success) oldest-first with SELECT ... FOR UPDATE SKIP LOCKED, so a
-- crash just releases the lock and the batch is retried (at-least-once). Retry
-- accounting / poison-message and lease columns (attempts, locked_at) are added
-- with the worker's retry policy -- omitted here to keep the first cut lean.
CREATE TABLE IF NOT EXISTS health_ingest_staging (
    -- Surrogate row handle the worker claims then DELETEs by. Unlike health_facts
    -- (which has a natural composite key), these rows have no reliable natural key
    -- -- upload_id is nullable and received_at isn't unique -- so a single-row
    -- delete needs this. Nothing else references it; it is not for ordering
    -- (received_at) or dedup (upload_id).
    id           BIGINT       GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id      BIGINT       NOT NULL,
    upload_id    VARCHAR(64),               -- client batch id for idempotent re-upload; NULL if none
    origin       SMALLINT     NOT NULL,     -- health::timeseries::Origin (source class of the batch)
    format       SMALLINT     NOT NULL,     -- health::timeseries::IngestFormat: the decode recipe (shape + codec)
                                           -- the worker must handle. Append-only enum, so a worker claims only
                                           -- rows with format <= the max its build supports (rolling-upgrade safe).
    payload      BYTEA        NOT NULL,     -- the uploaded batch verbatim, opaque bytes (JSON, or compressed / binary)
    received_at  BIGINT       NOT NULL,     -- unix milliseconds, queue order (app-stamped)
    -- Idempotent re-upload: a repeated (user, upload_id) can't double-insert. A
    -- NULL upload_id is exempt (SQL NULLs are distinct), so batches with no client
    -- id are never deduped here.
    UNIQUE (user_id, upload_id)
);

-- The worker's claim scan: pending batches, oldest first.
CREATE INDEX IF NOT EXISTS idx_health_ingest_staging_received
    ON health_ingest_staging (received_at);


------------------------------------------------------------------------------
-- What fhir_resources.content looks like, and how each shape projects into
-- health_facts. FHIR is R5 (see src/health/vendor/platform/vitalera.cpp); the
-- fields below are R4/R5-common. `(1<<60)|N` is the packed fhir_id of a LOINC
-- code with its dash stripped (indicator/fhir_id.hpp); LOINC system = 1.
------------------------------------------------------------------------------
--
-- (1) DISCRETE LAB VALUE (numeric, point) -- fasting glucose:
--   {
--     "resourceType": "Observation", "id": "obs-glucose-8f3a", "status": "final",
--     "code": { "coding": [{ "system": "http://loinc.org", "code": "1558-6",
--                            "display": "Fasting glucose ... in Serum or Plasma" }] },
--     "subject": { "reference": "Patient/123" },
--     "effectiveDateTime": "2026-06-30T08:12:00Z",
--     "valueQuantity": { "value": 6.1, "unit": "mmol/L",
--                        "system": "http://unitsofmeasure.org", "code": "mmol/L" }
--   }
--   -> 1 fact: indicator_id=(1<<60)|15586, value_num=6.1, unit='mmol/L',
--      effective_start=effective_end=<ms of 08:12Z>, origin=3, source_id='obs-glucose-8f3a'.
--
-- (2) HIGH-FREQUENCY STREAM, DOWNSAMPLED TO A DAILY BUCKET -- mean heart rate.
--     Note effectivePeriod (the bucket span), not effectiveDateTime: one
--     Observation = one bucket, so ~86,400 raw samples collapse to 1 row.
--   {
--     "resourceType": "Observation", "id": "obs-hr-2026-07-01-daily", "status": "final",
--     "code": { "coding": [{ "system": "http://loinc.org", "code": "8867-4",
--                            "display": "Heart rate" }] },
--     "subject": { "reference": "Patient/123" },
--     "effectivePeriod": { "start": "2026-07-01T00:00:00Z", "end": "2026-07-01T23:59:59Z" },
--     "valueQuantity": { "value": 74, "unit": "beats/minute",
--                        "system": "http://unitsofmeasure.org", "code": "/min" }
--   }
--   -> 1 fact (not 86,400): indicator_id=(1<<60)|88674, value_num=74,
--      effective_start=<ms of day start>, effective_end=<ms of day end>, origin=1(device).
--      (min/max/count, if kept, ride as `component` entries -- see (3)'s shape.)
--
-- (3) COMPOUND VALUE, SPLIT INTO ONE FACT PER COMPONENT -- blood pressure:
--   {
--     "resourceType": "Observation", "id": "obs-bp-77c2", "status": "final",
--     "code": { "coding": [{ "system": "http://loinc.org", "code": "85354-9",
--                            "display": "Blood pressure panel" }] },
--     "subject": { "reference": "Patient/123" },
--     "effectiveDateTime": "2026-07-01T09:03:00Z",
--     "component": [
--       { "code": { "coding": [{ "system": "http://loinc.org", "code": "8480-6",
--                                "display": "Systolic BP" }] },
--         "valueQuantity": { "value": 120, "unit": "mmHg", "code": "mm[Hg]" } },
--       { "code": { "coding": [{ "system": "http://loinc.org", "code": "8462-4",
--                                "display": "Diastolic BP" }] },
--         "valueQuantity": { "value": 80, "unit": "mmHg", "code": "mm[Hg]" } }
--     ]
--   }
--   -> 2 facts, same source_id='obs-bp-77c2' and same effective_start/end (an
--      instant, so end==start), distinct indicator_id: systolic (1<<60)|84806
--      value_num=120; diastolic (1<<60)|84624 value_num=80. The PK includes
--      indicator_id, so no collision.
--
-- (4) NON-NUMERIC RESULT (LOINC can't express the value -> SNOMED CT in the
--     value) -- hepatitis B surface antigen, qualitative:
--   {
--     "resourceType": "Observation", "id": "obs-hbsag-4d19", "status": "final",
--     "code": { "coding": [{ "system": "http://loinc.org", "code": "5195-3",
--                            "display": "Hepatitis B virus surface Ag [Presence] in Serum" }] },
--     "subject": { "reference": "Patient/123" },
--     "effectiveDateTime": "2026-06-30T08:12:00Z",
--     "valueCodeableConcept": { "coding": [{ "system": "http://snomed.info/sct",
--                               "code": "10828004", "display": "Positive (qualifier value)" }],
--                               "text": "阳性" }
--   }
--   -> 1 fact: indicator_id=(1<<60)|51953 (the indicator is still LOINC),
--      value_num=NULL, value_text='阳性', effective_start=effective_end=<ms>.
--   This shows LOINC-first / SNOMED-fallback at TWO places: the indicator (code)
--   prefers LOINC; the VALUE falls back to SNOMED via valueCodeableConcept when
--   LOINC's quantity can't hold it (-> value_text). An indicator that itself has
--   no LOINC (a clinical finding) would carry code.coding.system=SNOMED and
--   health_indicators.system=0 -- code_to_fhir_id packs numeric SCTIDs too.
