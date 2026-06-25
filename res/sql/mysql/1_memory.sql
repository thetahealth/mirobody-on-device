-- Long-term memory: durable per-user facts and their embeddings, recalled by
-- the memory module (src/memory/) for the `remember` / `recall_memory` MCP
-- tools. The local backend stores each embedding as a little-endian float32
-- BLOB (1024 floats for the shipped embedders) and ranks a user's rows by
-- in-process cosine similarity -- a per-user query loads only that user's rows,
-- so no vector index is needed. `created_at` is unix milliseconds (the app stamps
-- it), matching the int64 the module reads back.
CREATE TABLE IF NOT EXISTS memories (
    id          BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id     INT          NOT NULL,
    kind        SMALLINT     NOT NULL DEFAULT 1,         -- database::MemoryKind (1=fact)
    content     TEXT         NOT NULL,
    embedding   LONGBLOB,                                -- little-endian float32 vector
    conversation_id  BIGINT,                             -- conversation it was captured in (conversations.id); NULL if none
    created_at  BIGINT       NOT NULL,                   -- unix milliseconds (app-stamped)
    updated_at  BIGINT,                                  -- unix milliseconds; set on update, NULL until first
    deleted_at  BIGINT,                                  -- soft delete: NULL = active, unix milliseconds = when forgotten
    KEY idx_memories_user (user_id)
);
