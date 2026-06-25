-- User profile. Server holds all sync'd users.
-- MySQL port of res/sql/pg/1_user.sql -- same semantics, dialect differences:
-- AUTO_INCREMENT ids, BIGINT unix-millisecond timestamps (app-stamped),
-- TINYINT(1) booleans, VARCHAR for the client IP, login_method/kind as SMALLINT
-- (the LoginMethod / McpKind enums in src/database/enums.hpp). Indexes are
-- declared inline -- MySQL has no CREATE INDEX IF NOT EXISTS.
CREATE TABLE IF NOT EXISTS users (
    id          INT          NOT NULL AUTO_INCREMENT PRIMARY KEY,
    email       VARCHAR(256),                                              -- contact email & email-login id; nullable
    name        VARCHAR(256) NOT NULL DEFAULT '',                          -- display name
    language    VARCHAR(16)  NOT NULL DEFAULT 'en',                        -- BCP 47 tag, e.g. 'en', 'zh-CN'
    timezone    VARCHAR(64)  NOT NULL DEFAULT 'America/Los_Angeles',       -- IANA timezone
    created_at  BIGINT       NOT NULL,                                     -- unix milliseconds (app-stamped)
    updated_at  BIGINT,                                                    -- unix milliseconds; set on update, NULL until first
    deleted_at  BIGINT,                                                    -- soft delete: NULL = active, unix milliseconds = when deleted
    -- Email unique only among non-deleted rows. MySQL has no partial index, so a
    -- functional index (8.0.13+) keys active rows by email and soft-deleted rows
    -- by NULL (NULLs are not unique) -- re-registration after soft delete works.
    UNIQUE KEY idx_uni_users_email_active ((CASE WHEN deleted_at IS NULL THEN email END))
);

------------------------------------------------------------------------------

-- External (OAuth) identities linked to a user (see pg/1_user.sql for the model).
CREATE TABLE IF NOT EXISTS user_identities (
    id            BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id       INT          NOT NULL,
    login_method  SMALLINT     NOT NULL,                                   -- LoginMethod enum (src/database/enums.hpp)
    provider_uid  VARCHAR(256) NOT NULL,                                   -- stable per-provider id (apple sub, wechat openid, ...)
    email         VARCHAR(256),                                            -- email provider reported (may differ from users.email)
    created_at    BIGINT       NOT NULL,                                   -- unix milliseconds (app-stamped)
    UNIQUE KEY idx_uni_user_identities (login_method, provider_uid),       -- one external account links to at most one user
    KEY idx_user_identities_user (user_id),
    CONSTRAINT fk_user_identities_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
);

------------------------------------------------------------------------------

-- Login history: one append-only row per login. Immutable, so no updated_at.
CREATE TABLE IF NOT EXISTS user_login_logs (
    id            BIGINT      NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id       INT         NOT NULL,
    login_method  SMALLINT    NOT NULL,                                    -- LoginMethod enum
    ip            VARCHAR(45),                                             -- client IP (IPv4 or IPv6), text
    user_agent    TEXT,                                                    -- client User-Agent
    created_at    BIGINT      NOT NULL,                                    -- login time, unix milliseconds (app-stamped)
    KEY idx_user_login_logs_user_time (user_id, created_at DESC),
    CONSTRAINT fk_user_login_logs_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
);

------------------------------------------------------------------------------

-- MCP access log: one append-only row per tool/resource/prompt/skill access.
CREATE TABLE IF NOT EXISTS user_mcp_access_logs (
    id            BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id       INT          NOT NULL,
    kind          SMALLINT     NOT NULL,                                   -- McpKind enum: 1=tool 2=resource 3=prompt 4=skill
    name          VARCHAR(128) NOT NULL,                                   -- tool name, resource URI, prompt/skill name
    is_success    TINYINT(1)   NOT NULL,                                   -- 0 / 1
    duration_ms   INT,                                                     -- latency in milliseconds
    created_at    BIGINT       NOT NULL,                                   -- access time, unix milliseconds (app-stamped)
    KEY idx_user_mcp_access_logs_user_time (user_id, created_at DESC),
    CONSTRAINT fk_user_mcp_access_logs_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
);
