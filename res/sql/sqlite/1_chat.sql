-- Chat history: conversations and their messages. The on-device DB holds only
-- the phone owner's conversations. A conversation row is thin -- just owner and
-- a summary -- because the per-turn detail lives on the message rows. Its `id`
-- is NOT the auto-assigned rowid: it equals the id of the conversation's opening
-- question (a messages row), so a single id names the thread across both tables
-- (persist_history inserts that question first, then seeds this row with its id).
-- The legacy backend (res/sql/pg_legacy) keeps th_sessions with a string
-- session_id key, so persist_history inserts the key explicitly there; the
-- history list/delete queries bridge the differing table and key-column names
-- (see MIROBODY_CONVERSATIONS_TABLE / MIROBODY_CONVERSATION_ID_COL in chat/chat.hpp).
CREATE TABLE IF NOT EXISTS conversations (
    id          INTEGER   NOT NULL PRIMARY KEY,       -- = the opening question's messages.id (supplied, not auto-assigned)
    user_id     INTEGER,
    summary     TEXT,
    created_at  INTEGER   NOT NULL,                   -- unix milliseconds (app-stamped)
    updated_at  INTEGER,                              -- unix milliseconds; set on update, NULL until first
    deleted_at  INTEGER                               -- soft delete: NULL = active, unix milliseconds = when deleted
);

CREATE INDEX IF NOT EXISTS idx_conversations_user_id ON conversations (user_id);

------------------------------------------------------------------------------

-- One row per message, not per turn: a single user question fans out to one
-- response row per answering agent (agents may respond simultaneously), so a
-- turn is a question row plus N response rows that point back at it via
-- question_id. role distinguishes the question (user) from the answers
-- (assistant); agent names which agent produced a given answer.
CREATE TABLE IF NOT EXISTS messages (
    id               INTEGER   NOT NULL PRIMARY KEY,        -- rowid alias, auto-assigned
    user_id          INTEGER   NOT NULL,
    conversation_id  INTEGER,                               -- the conversation (= its opening question's id); NULL on the opening question itself (it is the root)
    question_id      INTEGER,                               -- response -> the question row it answers; NULL on question rows
    role             INTEGER   NOT NULL,                    -- database::MessageRole
    agent            TEXT,                                  -- agent that produced a response row; NULL for user questions
    provider         TEXT,                                  -- LLM provider behind that agent's response; NULL for user questions
    content          TEXT,
    language         TEXT,                                  -- per-question request hint; NULL on response rows
    timezone         TEXT,                                  -- per-question IANA zone; NULL on response rows
    files            TEXT,                                  -- per-question attachments: JSON array of {name, key}; NULL = none
    created_at       INTEGER   NOT NULL,                    -- unix milliseconds (app-stamped)
    deleted_at       INTEGER                                -- soft delete: NULL = active, unix milliseconds = when deleted
);

-- Index the read paths: a whole conversation in chronological order, the fan-out
-- of responses to one question, and per user. Drop the pre-rename names.
CREATE INDEX IF NOT EXISTS idx_messages_conversation_id ON messages (conversation_id, created_at);
CREATE INDEX IF NOT EXISTS idx_messages_question_id     ON messages (question_id);
CREATE INDEX IF NOT EXISTS idx_messages_user_id         ON messages (user_id);

------------------------------------------------------------------------------

-- Per-user uploaded files: a queryable index over the chat upload path's
-- objects, so /api/files can sort (by upload time or filename), search, and
-- paginate across a user's whole history. SQLite port of res/sql/pg/1_chat.sql
-- (same semantics; INTEGER id/booleans, INTEGER unix-ms timestamps the app
-- stamps -- created_at on insert, updated_at on update). The object-storage
-- .meta sidecars stay the durable source of truth; this table is a secondary
-- index. The upload path inserts the row, then UPDATEs text_key once extraction
-- finishes (and summary later, as further processing lands; both NULL until
-- then). file_key is the prefix-less object key, unique among active rows
-- (content-addressed): a re-upload upserts rather than duplicates.
CREATE TABLE IF NOT EXISTS files (
    id            INTEGER   NOT NULL PRIMARY KEY,              -- rowid alias, auto-assigned
    user_id       INTEGER   NOT NULL,
    file_key      TEXT      NOT NULL,
    filename      TEXT,
    content_type  TEXT,
    text_key      TEXT,                                        -- .trans key; NULL until extracted / none
    summary       TEXT,                                        -- AI summary; NULL until post-upload processing
    size_bytes    INTEGER   NOT NULL DEFAULT 0,
    created_at    INTEGER   NOT NULL,                          -- upload time, unix milliseconds (app-stamped); sort key
    updated_at    INTEGER,                                     -- unix milliseconds; set on update, NULL until first
    deleted_at    INTEGER                                      -- soft delete: NULL = active, unix milliseconds = when deleted
);

-- file_key unique only among active rows: a re-upload after soft delete starts a
-- fresh row (the deleted one stays as history), mirroring users.email.
CREATE UNIQUE INDEX IF NOT EXISTS uq_files_file_key_active
    ON files (file_key) WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_files_user_created  ON files (user_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_files_user_filename ON files (user_id, filename);
