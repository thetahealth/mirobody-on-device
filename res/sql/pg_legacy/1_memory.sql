-- Long-term memory: durable per-user facts and their embeddings, recalled by
-- the memory module (src/memory/) for the `remember` / `recall_memory` MCP
-- tools. New mirobody-v2 feature -- the legacy Python stack does not read it --
-- so it uses the module's native int64 user_id (BIGINT) rather than the legacy
-- VARCHAR user ids. The local backend stores each embedding as a little-endian
-- float32 BYTEA and ranks a user's rows by in-process cosine similarity (no
-- vector index; a per-user query loads only that user's rows). `created_at` is
-- unix milliseconds, matching the int64 the module reads back.
CREATE TABLE IF NOT EXISTS memories (
    id          BIGSERIAL    PRIMARY KEY,
    user_id     BIGINT       NOT NULL,
    kind        SMALLINT     NOT NULL DEFAULT 1,         -- database::MemoryKind (1=fact)
    content     TEXT         NOT NULL,
    embedding   BYTEA,                                   -- little-endian float32 vector
    conversation_id  BIGINT,                             -- conversation it was captured in (conversations.id); NULL if none
    created_at  BIGINT       NOT NULL,                   -- unix milliseconds (app-stamped)
    updated_at  BIGINT,                                  -- unix milliseconds; set on update, NULL until first
    deleted_at  BIGINT                                   -- soft delete: NULL = active, unix milliseconds = when forgotten
);

CREATE INDEX IF NOT EXISTS idx_memories_user_id ON memories (user_id);
