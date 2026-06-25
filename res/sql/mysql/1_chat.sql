-- Chat history: conversations and their messages, the modern shape (matching
-- res/sql/pg/1_chat.sql). A conversation row is thin -- just owner and a summary --
-- because the per-turn detail lives on the message rows. Its `id` is NOT
-- auto-increment: it equals the id of the conversation's opening question (a
-- messages row), so a single id names the thread across both tables
-- (persist_history inserts that question first, then seeds this row with its id).
-- On MySQL the chat persistence code (persist_history / history list / delete)
-- runs its non-legacy path, so it uses this `conversations` table keyed on the
-- BIGINT `id` (see MIROBODY_CONVERSATIONS_TABLE / MIROBODY_CONVERSATION_ID_COL
-- in chat/chat.hpp).
--
-- The `files` upload index from res/sql/pg/1_chat.sql is intentionally NOT ported
-- here: its DAO relies on `ON CONFLICT (file_key) WHERE deleted_at IS NULL` plus a
-- partial unique index, and MySQL supports neither (it uses ON DUPLICATE KEY UPDATE
-- and has no filtered indexes). A working MySQL `files` needs a MySQL-specific
-- upsert in transcode/file.cpp, not just schema -- so /api/files stays unsupported
-- on MySQL until that lands.
CREATE TABLE IF NOT EXISTS conversations (
    id          BIGINT      NOT NULL PRIMARY KEY,            -- = the opening question's messages.id (supplied, not auto-increment)
    user_id     INT,
    summary     TEXT,
    created_at  BIGINT      NOT NULL,                        -- unix milliseconds (app-stamped)
    updated_at  BIGINT,                                      -- unix milliseconds; set on update, NULL until first
    deleted_at  BIGINT,                                      -- soft delete: NULL = active, unix milliseconds = when deleted
    KEY idx_conversations_user_id (user_id)
);

------------------------------------------------------------------------------

-- One row per message, not per turn: a single user question fans out to one
-- response row per answering agent (agents may respond simultaneously), so a
-- turn is a question row plus N response rows that point back at it via
-- question_id. role distinguishes the question (user) from the answers
-- (assistant); agent names which agent produced a given answer.
CREATE TABLE IF NOT EXISTS messages (
    id               BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id          INT          NOT NULL,
    conversation_id  BIGINT,                                   -- the conversation (= its opening question's id); NULL on the opening question itself (it is the root)
    question_id      BIGINT,                                   -- response -> the question row it answers; NULL on question rows
    role             SMALLINT     NOT NULL,                    -- database::MessageRole
    agent            VARCHAR(64),                              -- agent that produced a response row; NULL for user questions
    provider         VARCHAR(64),                              -- LLM provider behind that agent's response; NULL for user questions
    content          TEXT,
    language         VARCHAR(64),                              -- per-question request hint; NULL on response rows
    timezone         VARCHAR(64),                              -- per-question IANA zone; NULL on response rows
    files            TEXT,                                     -- per-question attachments: JSON array of {name, key}; NULL = none
    created_at       BIGINT       NOT NULL,                    -- unix milliseconds (app-stamped)
    deleted_at       BIGINT,                                   -- soft delete: NULL = active, unix milliseconds = when deleted
    KEY idx_messages_conversation_id (conversation_id, created_at),
    KEY idx_messages_question_id (question_id),
    KEY idx_messages_user_id (user_id)
);
