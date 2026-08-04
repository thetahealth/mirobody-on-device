-- Health data store. SQLite port of res/sql/pg/1_health.sql -- same semantics,
-- dialect differences: TEXT strings, INTEGER unix-millisecond timestamps the app
-- stamps, INTEGER booleans.

-- FHIR R5 resources, one row per (user, type, id). content holds the resource
-- JSON; the lifecycle columns beside it: version_id (FHIR meta.versionId),
-- updated_at (last write, unix ms; search sort + Last-Modified header -- FHIR
-- meta.lastUpdated, the ISO instant, lives inside content), deleted_at (soft
-- delete: NULL = live, unix ms = when deleted; read -> 410 Gone).
CREATE TABLE IF NOT EXISTS fhir_resources (
    user_id       INTEGER NOT NULL,
    resource_type TEXT    NOT NULL,
    resource_id   TEXT    NOT NULL,

    version_id    INTEGER NOT NULL DEFAULT 1,
    updated_at    INTEGER NOT NULL,                 -- unix milliseconds (app-stamped)
    deleted_at    INTEGER,                          -- soft delete: NULL = live, unix ms = when deleted
    content       TEXT    NOT NULL,
    PRIMARY KEY (user_id, resource_type, resource_id)
);

------------------------------------------------------------------------------

-- Links a mirobody user to their account/patient id on an external health-data
-- vendor (see src/vendor/). One row per (user, vendor): external_user_id is the
-- id the vendor assigns -- a Vitalera patient id, a Terra user UUID -- that
-- Vendor::fetch is later called with. No FK on user_id, matching fhir_resources
-- above (the user table name differs by schema: users vs health_app_user).
--
-- verified_at gates data access: the fetch path refuses to call the vendor until
-- the link is proven to belong to the user (NULL = pending). The UNIQUE on
-- (vendor_id, external_user_id) is the ownership backstop -- one vendor account
-- links to at most one mirobody user, so A cannot bind B's patient id (mirrors
-- user_identities' UNIQUE(login_method, provider_uid)).
CREATE TABLE IF NOT EXISTS user_vendor_accounts (
    user_id          INTEGER NOT NULL,
    vendor_id        TEXT    NOT NULL,                 -- stable vendor key: 'vitalera', 'terra', ...
    external_user_id TEXT    NOT NULL,                 -- the vendor's id for this user (consent/provision output)
    created_at       INTEGER NOT NULL,                 -- unix milliseconds (app-stamped)
    updated_at       INTEGER,                          -- last modification, unix ms (bind / verify / token write)
    verified_at      INTEGER,                          -- ownership confirmed, unix ms; NULL = pending (no fetch allowed)
    -- Per-user OAuth tokens, Fernet-encrypted at rest (VENDOR_TOKEN_ENCRYPTION_KEY).
    -- NULL until stored; the server pulls each user's data with these and refreshes
    -- the access token via the vendor's refresh grant when it expires.
    access_token     TEXT,                             -- encrypted OAuth access token
    refresh_token    TEXT,                             -- encrypted OAuth refresh token (may be absent)
    token_expires_at INTEGER,                          -- access token expiry, unix ms; NULL = unknown/non-expiring
    -- PK covers per-(user,vendor) lookup; the UNIQUE both enforces single
    -- ownership and indexes the reverse (vendor account -> user) lookup.
    PRIMARY KEY (user_id, vendor_id),
    UNIQUE (vendor_id, external_user_id)
);
