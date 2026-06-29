-- User profile. Server holds all sync'd users; mobile DB holds only the phone owner.
CREATE TABLE IF NOT EXISTS users (
    id            INTEGER      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    -- primary/contact email & email-login id; nullable (provider-only signup may
    -- lack one); unique among active rows (see index below).
    email         VARCHAR(256),
    name          VARCHAR(256) NOT NULL DEFAULT '',           -- display name
    language      VARCHAR(16)  NOT NULL DEFAULT 'en',         -- BCP 47 tag, e.g. 'en', 'zh-CN'
    timezone      VARCHAR(64)  NOT NULL DEFAULT 'America/Los_Angeles',  -- IANA timezone
    created_at    BIGINT       NOT NULL,                      -- unix milliseconds (app-stamped)
    updated_at    BIGINT,                                     -- unix milliseconds; set on update, NULL until first
    deleted_at    BIGINT                                      -- soft delete: NULL = active, unix milliseconds = when deleted
);

-- Email unique only among non-deleted rows: re-registration after soft delete is allowed.
CREATE UNIQUE INDEX IF NOT EXISTS idx_uni_users_email_active
    ON users (email) WHERE deleted_at IS NULL;

-- Examples:
--   INSERT INTO users (email, name) VALUES ('a@x.com', 'Alice');  -- email signup
--   INSERT INTO users (name) VALUES ('Bob');                      -- provider-only (email NULL)
--   UPDATE users SET deleted_at = 1718000000000 WHERE id = 1;  -- soft delete (unix ms)
--   SELECT * FROM users WHERE deleted_at IS NULL;                  -- active users only

------------------------------------------------------------------------------

-- External (OAuth) identities linked to a user. A user may link many; linking is
-- explicit (done while logged in). Email/magic-link login resolves via users.email,
-- not this table.
CREATE TABLE IF NOT EXISTS user_identities (
    id              BIGINT       GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id         INTEGER      NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    login_method    SMALLINT     NOT NULL,  -- LoginMethod enum, see src/database/enums.hpp
    provider_uid    VARCHAR(256) NOT NULL,  -- stable per-provider id (apple sub, wechat openid, ...)
    email           VARCHAR(256),           -- email provider reported (may differ from users.email)
    created_at      BIGINT       NOT NULL,  -- unix milliseconds (app-stamped)
    UNIQUE (login_method, provider_uid)     -- one external account links to at most one user
);

-- List all identities for a user (account settings, unlinking).
CREATE INDEX IF NOT EXISTS idx_user_identities_user
    ON user_identities (user_id);

-- Examples (login_method is the LoginMethod enum, see src/database/enums.hpp; 2 = apple):
--   -- link a provider while logged in (user_id from session)
--   INSERT INTO user_identities (user_id, login_method, provider_uid, email)
--   VALUES (1, 2, '000123.abc', 'a@privaterelay.appleid.com');
--   -- resolve a login: which user owns this external account?
--   SELECT user_id FROM user_identities WHERE login_method = 2 AND provider_uid = '000123.abc';
--   -- a user's linked identities (account settings / unlinking)
--   SELECT * FROM user_identities WHERE user_id = 1;

------------------------------------------------------------------------------

-- Login history: one append-only row per login attempt. Immutable, so no updated_at/trigger.
CREATE TABLE IF NOT EXISTS user_login_logs (
    id              BIGINT      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id         INTEGER     NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    login_method    SMALLINT    NOT NULL,                       -- LoginMethod enum, see src/database/enums.hpp
    ip              INET,                                       -- client IP (IPv4 or IPv6)
    user_agent      TEXT,                                       -- client User-Agent
    created_at      BIGINT      NOT NULL                        -- login time, unix milliseconds (app-stamped)
);

-- Fetch a user's logins newest-first.
CREATE INDEX IF NOT EXISTS idx_user_login_logs_user_time
    ON user_login_logs (user_id, created_at DESC);

-- Examples:
--   INSERT INTO user_login_logs (user_id, login_method, ip, user_agent)
--   VALUES (1, 2, '203.0.113.7', 'Mozilla/5.0 ...');   -- login_method 2 = apple
--   -- a user's recent logins (uses idx_user_login_logs_user_time)
--   SELECT * FROM user_login_logs WHERE user_id = 1 ORDER BY created_at DESC LIMIT 20;

------------------------------------------------------------------------------

-- MCP access log: one append-only row per access of a tool/resource/prompt/skill.
-- Immutable, so no updated_at/trigger. Metadata only -- no request/response payload is stored.
CREATE TABLE IF NOT EXISTS user_mcp_access_logs (
    id              BIGINT       GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id         INTEGER      NOT NULL REFERENCES users (id) ON DELETE CASCADE,
    -- McpKind enum (src/database/enums.hpp): 1=tool 2=resource 3=prompt 4=skill
    kind            SMALLINT     NOT NULL,
    name            VARCHAR(128) NOT NULL,        -- which one: tool name, resource URI, prompt/skill name
    is_success      BOOLEAN      NOT NULL,        -- did the access succeed?
    duration_ms     INTEGER,                      -- latency in milliseconds
    created_at      BIGINT       NOT NULL         -- access time, unix milliseconds (app-stamped)
);

-- Fetch a user's MCP accesses newest-first.
CREATE INDEX IF NOT EXISTS idx_user_mcp_access_logs_user_time
    ON user_mcp_access_logs (user_id, created_at DESC);

-- Examples (kind is the McpKind enum, see src/database/enums.hpp):
--   INSERT INTO user_mcp_access_logs (user_id, kind, name, is_success, duration_ms)
--   VALUES (1, 1, 'search_users', TRUE, 42);   -- kind 1 = tool
--   -- a user's recent MCP accesses
--   SELECT * FROM user_mcp_access_logs WHERE user_id = 1 ORDER BY created_at DESC LIMIT 20;
--   -- usage counts per resource (kind 2 = resource)
--   SELECT name, count(*) FROM user_mcp_access_logs WHERE kind = 2 GROUP BY name;
