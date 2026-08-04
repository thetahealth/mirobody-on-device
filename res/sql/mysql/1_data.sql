-- Health-data projection layer. MySQL port of res/sql/pg/1_data.sql -- same
-- semantics and design; dialect differences: VARCHAR/TEXT strings, DOUBLE,
-- LONGBLOB embedding, BIGINT unix-millisecond timestamps (app-stamped), and
-- indexes declared inline (MySQL has no CREATE INDEX IF NOT EXISTS). See the pg
-- file for the full rationale and the fhir_resources.content example shapes.
--
-- The query- and semantic-search-optimized read model derived from
-- fhir_resources (1_health.sql), the single source of truth: every row is
-- materialized on FHIR write and derivable from a fhir_resources row (via the
-- source_id back-pointer), so the whole layer can be rebuilt by replaying FHIR.
-- v2 successor to the Python th_series_dim (dimension) + th_series_data (facts)
-- split, minus terminology mapping: (system, code) is resolved at write time by
-- src/indicator, so this layer only stores. User health data is coded
-- LOINC-first, SNOMED CT only where LOINC can't express it -- a two-system space
-- small enough that the packed fhir_id keys every indicator.

-- health_indicators -- the indicator dimension, GLOBAL (concept metadata is
-- user-independent, so display / unit / embedding are stored once). Lazily
-- materialized only for indicators that appear in some user's data, so it stays
-- in the low thousands of rows: a per-query in-process cosine over that set is
-- enough and no ANN index is needed. The embedding is a little-endian float16
-- LONGBLOB (1024 halves = 2 KB; widened to float32 before the cosine). A native
-- vector column + ANN index is the upgrade path if the set ever outgrows an
-- in-process scan.
CREATE TABLE IF NOT EXISTS health_indicators (
    -- id is the packed (system, code) fhir_id (indicator/fhir_id.hpp), app-computed
    -- at projection time -- deterministic and stable across rebuilds, so facts stay
    -- linked (indicator_id = fhir_id) and each table re-derives from FHIR
    -- independently. Works because user data is scoped to LOINC + SNOMED CT (both
    -- system <= 1, numeric codes inside the 60-bit budget -- code_to_fhir_id's
    -- domain). Readable system/code kept alongside so display / FHIR round-trips
    -- need no unpack.
    id          BIGINT       NOT NULL PRIMARY KEY,  -- packed (system<<60)|code fhir_id (app-computed; not AUTO_INCREMENT)
    `system`    SMALLINT     NOT NULL,     -- indicator::System: 1=LOINC (primary), 0=SNOMED_CT (fallback)
    code        VARCHAR(64)  NOT NULL,     -- the vocabulary's own code string ("2345-7", SNOMED SCTID)
    display     VARCHAR(200) NOT NULL,     -- canonical display name (e.g. "空腹血糖 / Fasting glucose")
    category    VARCHAR(200),              -- grouping for UI/agent (血糖 / 血脂 / 生命体征 / …)
    unit        VARCHAR(64),               -- canonical unit (fhir/units normalized)
    embedding   LONGBLOB,                  -- little-endian float16 vector (1024 halves); NULL until embedded
    created_at  BIGINT       NOT NULL,     -- unix milliseconds (app-stamped)
    updated_at  BIGINT                     -- unix milliseconds; set on re-embed / metadata refresh, NULL until first
    -- No separate UNIQUE(system, code): id is a bijection of that pair, so
    -- PRIMARY KEY(id) already enforces one row per concept.
);

------------------------------------------------------------------------------

-- health_facts -- the flattened observations (numeric value + clinical time),
-- scoped per user. Successor to th_series_data. No embedding of its own: semantic
-- recall hits health_indicators first, then reads its facts here by
-- (user_id, indicator_id[, effective_start]). Each row back-points to the source
-- Observation (source_id) -- the sync/rebuild anchor.
--
-- effective_start/effective_end map FHIR effectiveDateTime (instant: end==start)
-- and effectivePeriod (start,end); the range carries the span, so no period enum
-- (aligned rollup bucket, irregular interval, and point reading are one shape).
--
-- Value is two nullable columns, exactly one set: value_num for the numeric
-- majority (SQL trends/thresholds), value_text for non-numeric ("阳性", coded).
-- "value_num IS NOT NULL" is the discriminator. Compound observations split into
-- one fact per component; full fidelity stays in fhir_resources.
CREATE TABLE IF NOT EXISTS health_facts (
    user_id       BIGINT           NOT NULL,
    indicator_id  BIGINT           NOT NULL,   -- -> health_indicators.id (the packed (system, code) fhir_id)
    value_num     DOUBLE,                      -- numeric reading (trends/charts); NULL for non-numeric
    value_text    TEXT,                        -- non-numeric result verbatim; NULL for pure numeric
    unit          VARCHAR(64),                 -- unit as stored on this reading (fhir/units normalized)
    effective_start BIGINT         NOT NULL,   -- observation's clinical time / bucket start, unix ms
    effective_end   BIGINT         NOT NULL,   -- bucket / interval end, unix ms; == effective_start for an instant
    origin        SMALLINT         NOT NULL,   -- health::timeseries::Origin: 1=device 2=ehr 3=document 4=manual
    source_id     VARCHAR(64)      NOT NULL,   -- back-pointer: fhir_resources.resource_id. No source_type
                                              -- column: only Observations project here, so resource_type is
                                              -- always 'Observation' (join back with it).
    deleted_at    BIGINT,                      -- soft delete: NULL = live, unix ms = when deleted
    created_at    BIGINT           NOT NULL,   -- unix milliseconds (app-stamped)
    updated_at    BIGINT,                      -- unix milliseconds; set on update, NULL until first
    -- Natural key = identity (no surrogate id: nothing references a single fact).
    -- Its leading prefix (user_id, indicator_id, effective_start) also serves the
    -- trend range scan, so no separate trend index. effective_end is payload.
    PRIMARY KEY (user_id, indicator_id, effective_start, source_id),
    -- Reverse lookup for the FHIR write/delete sync path (facts of a given source
    -- Observation). Inline -- MySQL has no CREATE INDEX IF NOT EXISTS.
    KEY idx_health_facts_source (source_id)
);

------------------------------------------------------------------------------

-- health_ingest_staging -- TRANSIENT ingest queue for raw high-frequency device
-- uploads (see pg/1_data.sql for the full rationale). Not part of the projection
-- and not durable storage: the hot path INSERTs a batch, the DuckDB worker drains
-- it (→ Parquet raw + fhir_resources summaries + health_facts) and DELETES the
-- row. Rows are transient; the table is permanent (NOT a SQL TEMPORARY table).
-- Only the raw wearable stream lands here; discrete data (labs / EHR / documents)
-- writes straight to fhir_resources.
CREATE TABLE IF NOT EXISTS health_ingest_staging (
    id           BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,  -- claim/delete handle; rows have no natural key
    user_id      BIGINT       NOT NULL,
    upload_id    VARCHAR(64),               -- client idempotency key (nonce per logical upload); NULL if none
    origin       SMALLINT     NOT NULL,     -- health::timeseries::Origin (source class of the batch)
    format       SMALLINT     NOT NULL,     -- health::timeseries::IngestFormat (decode recipe; append-only, worker claims format <= its max)
    payload      LONGBLOB     NOT NULL,     -- the uploaded batch verbatim, opaque bytes (JSON, or compressed / binary)
    received_at  BIGINT       NOT NULL,     -- unix milliseconds, queue order (app-stamped)
    -- Idempotent re-upload: a repeated (user, upload_id) can't double-insert; a
    -- NULL upload_id is exempt (SQL NULLs are distinct).
    UNIQUE KEY idx_uni_health_ingest_upload (user_id, upload_id),
    -- worker's claim scan: pending batches, oldest first
    KEY idx_health_ingest_staging_received (received_at)
);
