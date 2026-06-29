-- Chat history: conversations and their messages. A conversation row is thin --
-- just owner and a summary -- because the per-turn detail lives on the message
-- rows. Its `id` is NOT auto-assigned: it equals the id of the conversation's
-- opening question (a messages row), so a single id names the thread across both
-- tables (persist_history inserts that question first, then seeds this row with
-- its id). The legacy backend (res/sql/pg_legacy) keeps th_sessions with a
-- string session_id key instead, so persist_history inserts the key explicitly
-- there; the history list/delete queries bridge the differing table and
-- key-column names (see MIROBODY_CONVERSATIONS_TABLE / MIROBODY_CONVERSATION_ID_COL
-- in chat/chat.hpp).
CREATE TABLE IF NOT EXISTS conversations (
    id          BIGINT       PRIMARY KEY,                 -- = the opening question's messages.id (not auto-assigned)
    user_id     BIGINT,
    summary     TEXT,
    created_at  BIGINT       NOT NULL,         -- unix milliseconds (app-stamped)
    updated_at  BIGINT,                                  -- unix milliseconds; set on update, NULL until first
    deleted_at  BIGINT                                   -- soft delete: NULL = active, unix milliseconds = when deleted
);

CREATE INDEX IF NOT EXISTS idx_conversations_user_id ON conversations (user_id);

------------------------------------------------------------------------------

-- One row per message, not per turn: a single user question fans out to one
-- response row per answering agent (agents may respond simultaneously), so a
-- turn is a question row plus N response rows that point back at it via
-- question_id. role distinguishes the question (user) from the answers
-- (assistant); agent names which agent produced a given answer.
CREATE TABLE IF NOT EXISTS messages (
    id               BIGINT       GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id          BIGINT       NOT NULL,
    conversation_id  BIGINT,                                -- the conversation (= its opening question's id); NULL on the opening question itself (it is the root)
    question_id      BIGINT,                                -- response -> the question row it answers; NULL on question rows
    role             SMALLINT     NOT NULL,                 -- database::MessageRole
    agent            VARCHAR(64),                           -- agent that produced a response row; NULL for user questions
    provider         VARCHAR(64),                           -- LLM provider behind that agent's response; NULL for user questions
    content          TEXT,
    language         VARCHAR(64),                           -- per-question request hint; NULL on response rows
    timezone         VARCHAR(64),                           -- per-question IANA zone; NULL on response rows
    files            TEXT,                                  -- per-question attachments: JSON array of {name, key}; NULL = none
    created_at       BIGINT       NOT NULL,       -- unix milliseconds (app-stamped)
    deleted_at       BIGINT                                 -- soft delete: NULL = active, unix milliseconds = when deleted
);

-- Index the read paths: a whole conversation in chronological order, the fan-out
-- of responses to one question, and per user. Drop the pre-rename name.
CREATE INDEX IF NOT EXISTS idx_messages_conversation_id ON messages (conversation_id, created_at);
CREATE INDEX IF NOT EXISTS idx_messages_question_id     ON messages (question_id);
CREATE INDEX IF NOT EXISTS idx_messages_user_id         ON messages (user_id);

------------------------------------------------------------------------------

-- Per-user uploaded files: a queryable index over the chat upload path's
-- objects, so /api/files can sort (by upload time or filename), search, and
-- paginate across a user's whole history -- which the cache + .meta sidecar
-- index (newest-first, capped) cannot. The object-storage .meta sidecars stay
-- the durable source of truth; this table is a secondary index, rebuildable
-- from them, so losing it is not data loss.
--
-- The upload path inserts the row first, then UPDATEs it as post-upload
-- processing finishes -- filling text_key (the .trans object key) once text is
-- extracted, and summary later as further processing lands. Both are NULL until
-- their step completes (or the file has none); the app stamps updated_at on
-- each touch. file_key is the prefix-less object key, unique among active rows
-- because it is content-addressed: a re-upload of identical bytes upserts this
-- row rather than duplicating it (created_at, the upload time, is kept).
-- Filenames are stored in the clear so ORDER BY / search work in SQL;
-- object-store encryption (FILE_ENCRYPTION_KEY) protects the bytes and the
-- bucket-side .meta, a separate trust domain from this database.
CREATE TABLE IF NOT EXISTS files (
    id            BIGINT       GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id       BIGINT       NOT NULL,
    file_key      VARCHAR(256) NOT NULL,
    filename      VARCHAR(256),
    content_type  VARCHAR(128),
    text_key      VARCHAR(256),                              -- .trans key; NULL until extracted / none
    summary       TEXT,                                      -- AI summary; NULL until post-upload processing fills it
    size_bytes    BIGINT       NOT NULL DEFAULT 0,
    created_at    BIGINT       NOT NULL,                     -- upload time, unix milliseconds (app-stamped); sort key
    updated_at    BIGINT,                                    -- unix milliseconds; set on update, NULL until first
    deleted_at    BIGINT                                     -- soft delete: NULL = active, unix milliseconds = when deleted
);

-- file_key unique only among active rows: a re-upload after soft delete starts a
-- fresh row (the deleted one stays as history), mirroring users.email.
CREATE UNIQUE INDEX IF NOT EXISTS uq_files_file_key_active
    ON files (file_key) WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_files_user_created  ON files (user_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_files_user_filename ON files (user_id, filename);
