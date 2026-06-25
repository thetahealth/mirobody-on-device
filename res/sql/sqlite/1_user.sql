-- User profile. Server holds all sync'd users; mobile DB holds only the phone owner.
-- SQLite port of res/sql/pg/1_user.sql -- same semantics, dialect differences:
-- INTEGER unix-millisecond timestamps the app stamps (created_at on insert,
-- updated_at on update), INTEGER booleans, login_method/kind as INTEGER (the
-- LoginMethod / McpKind enums in src/database/enums.hpp).
CREATE TABLE IF NOT EXISTS users (
    id          INTEGER   NOT NULL PRIMARY KEY,                 -- rowid alias, auto-assigned
    -- contact email & email-login id; nullable; unique among active rows.
    email       TEXT,
    name        TEXT      NOT NULL DEFAULT '',                  -- display name
    language    TEXT      NOT NULL DEFAULT 'en',                -- BCP 47 tag, e.g. 'en', 'zh-CN'
    timezone    TEXT      NOT NULL DEFAULT 'America/Los_Angeles',  -- IANA timezone
    created_at  INTEGER   NOT NULL,                             -- unix milliseconds (app-stamped)
    updated_at  INTEGER,                                        -- unix milliseconds; set on update, NULL until first
    deleted_at  INTEGER                                         -- soft delete: NULL = active, unix milliseconds = when deleted
);

-- Email unique only among non-deleted rows: re-registration after soft delete is allowed.
CREATE UNIQUE INDEX IF NOT EXISTS idx_uni_users_email_active
    ON users (email) WHERE deleted_at IS NULL;

------------------------------------------------------------------------------

-- External (OAuth) identities linked to a user (see pg/1_user.sql for the model).
CREATE TABLE IF NOT EXISTS user_identities (
    id            INTEGER NOT NULL PRIMARY KEY,
    user_id       INTEGER NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    login_method  INTEGER NOT NULL,                             -- LoginMethod enum (src/database/enums.hpp)
    provider_uid  TEXT    NOT NULL,                             -- stable per-provider id (apple sub, wechat openid, ...)
    email         TEXT,                                         -- email provider reported (may differ from users.email)
    created_at    INTEGER NOT NULL,                             -- unix milliseconds (app-stamped)
    UNIQUE (login_method, provider_uid)                         -- one external account links to at most one user
);

CREATE INDEX IF NOT EXISTS idx_user_identities_user
    ON user_identities (user_id);

------------------------------------------------------------------------------

-- Login history: one append-only row per login. Immutable, so no updated_at.
CREATE TABLE IF NOT EXISTS user_login_logs (
    id            INTEGER NOT NULL PRIMARY KEY,
    user_id       INTEGER NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    login_method  INTEGER NOT NULL,                             -- LoginMethod enum
    ip            TEXT,                                         -- client IP (IPv4 or IPv6)
    user_agent    TEXT,                                         -- client User-Agent
    created_at    INTEGER NOT NULL                              -- login time, unix milliseconds (app-stamped)
);

CREATE INDEX IF NOT EXISTS idx_user_login_logs_user_time
    ON user_login_logs (user_id, created_at DESC);

------------------------------------------------------------------------------

-- MCP access log: one append-only row per tool/resource/prompt/skill access.
CREATE TABLE IF NOT EXISTS user_mcp_access_logs (
    id            INTEGER NOT NULL PRIMARY KEY,
    user_id       INTEGER NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    kind          INTEGER NOT NULL,                             -- McpKind enum: 1=tool 2=resource 3=prompt 4=skill
    name          TEXT    NOT NULL,                             -- tool name, resource URI, prompt/skill name
    is_success    INTEGER NOT NULL,                             -- 0 / 1
    duration_ms   INTEGER,                                      -- latency in milliseconds
    created_at    INTEGER NOT NULL                              -- access time, unix milliseconds (app-stamped)
);

CREATE INDEX IF NOT EXISTS idx_user_mcp_access_logs_user_time
    ON user_mcp_access_logs (user_id, created_at DESC);
