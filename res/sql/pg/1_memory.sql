-- Long-term memory: durable per-user facts and their embeddings, recalled by
-- the memory module (src/memory/) for the `remember` / `recall_memory` MCP
-- tools. The local backend ranks a user's rows by in-process cosine similarity,
-- so the embedding is stored verbatim as a little-endian float32 BYTEA (1024
-- floats for the shipped embedders); a per-user query loads only that user's
-- rows, so no vector index is needed here. (A pgvector `vector(1024)` column
-- with an ANN index is the natural upgrade once one user accumulates tens of
-- thousands of memories; it slots in behind the same Memory interface.)
-- `created_at` is unix milliseconds (the app stamps it), matching the int64 the
-- module reads back.
CREATE TABLE IF NOT EXISTS memories (
    id          BIGINT       GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id     BIGINT       NOT NULL,
    kind        SMALLINT     NOT NULL DEFAULT 1,         -- database::MemoryKind (1=fact)
    content     TEXT         NOT NULL,
    embedding   BYTEA,                                   -- little-endian float32 vector
    conversation_id  BIGINT,                             -- conversation it was captured in (conversations.id); NULL if none
    created_at  BIGINT       NOT NULL,                   -- unix milliseconds (app-stamped)
    updated_at  BIGINT,                                  -- unix milliseconds; set on update, NULL until first
    deleted_at  BIGINT                                   -- soft delete: NULL = active, unix milliseconds = when forgotten
);

CREATE INDEX IF NOT EXISTS idx_memories_user_id ON memories(user_id);
