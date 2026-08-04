-- Long-term memory: durable per-user facts and their embeddings, recalled by
-- the memory module (src/memory/) for the `remember` / `recall_memory` MCP
-- tools. The on-device DB holds only the phone owner's memories. The local
-- backend ranks a user's rows by in-process cosine similarity, so the embedding
-- is stored verbatim as a little-endian float32 BLOB (1024 floats for the
-- shipped embedders) and there is no vector index -- a per-user query loads only
-- that user's rows. `created_at` is unix milliseconds (the app stamps it), matching
-- the int64 the module reads back.
CREATE TABLE IF NOT EXISTS memories (
    id          INTEGER NOT NULL PRIMARY KEY,    -- rowid alias, auto-assigned
    user_id     INTEGER NOT NULL,
    kind        INTEGER NOT NULL DEFAULT 1,      -- database::MemoryKind (1=fact)
    content     TEXT    NOT NULL,                -- the memory text
    embedding   BLOB,                            -- little-endian float32 vector; NULL if unembedded
    conversation_id  INTEGER,                    -- conversation it was captured in (conversations.id); NULL if none
    created_at  INTEGER NOT NULL,                -- unix milliseconds (app-stamped)
    updated_at  INTEGER,                         -- unix milliseconds; set on update, NULL until first
    deleted_at  INTEGER                          -- soft delete: NULL = active, unix milliseconds = when forgotten
);

CREATE INDEX IF NOT EXISTS idx_memories_user_id ON memories(user_id);
