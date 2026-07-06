CREATE TABLE IF NOT EXISTS series_data (
    user_id character varying not null,
    indicator character varying not null,
    source character varying,
    time timestamp without time zone not null,
    value text not null,
    create_time timestamp without time zone not null default now(),  -- matches test/prod (no tz)
    update_time timestamp with time zone not null default now(),
    timezone character varying(50),
    task_id character varying(200),
    source_id character varying(128),
    platform varchar(32) DEFAULT NULL,
    CONSTRAINT unique_series_data_user_indicator_source_time UNIQUE (user_id, indicator, source, time)
);

-- Create composite index on user_id and platform for better query performance
CREATE INDEX IF NOT EXISTS idx_series_data_user_platform
ON series_data(user_id, platform) WHERE platform IS NOT NULL;


CREATE TABLE IF NOT EXISTS th_series_data
(
    id integer generated always as identity not null,
    user_id character varying(200) COLLATE pg_catalog."default",
    indicator character varying(200) COLLATE pg_catalog."default",
    value text COLLATE pg_catalog."default",
    start_time timestamp without time zone,
    end_time timestamp without time zone,
    source_table character varying(200) COLLATE pg_catalog."default",
    source_table_id character varying(200) COLLATE pg_catalog."default",
    comment text COLLATE pg_catalog."default",
    indicator_id text COLLATE pg_catalog."default" NOT NULL DEFAULT ''::text,
    deleted integer NOT NULL DEFAULT 0,
    create_time timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    update_time timestamp with time zone DEFAULT CURRENT_TIMESTAMP,
    source character varying(128) COLLATE pg_catalog."default",
    task_id character varying(200) COLLATE pg_catalog."default",
    CONSTRAINT th_series_data_pkey PRIMARY KEY (id),
    CONSTRAINT unique_user_indicator_start_end_time UNIQUE (user_id, indicator, start_time, end_time)
);

CREATE INDEX IF NOT EXISTS idx_th_series_data_source_table_id ON th_series_data(source_table_id);


CREATE TABLE IF NOT EXISTS th_series_dim (
    id INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    original_indicator character varying(200) UNIQUE NOT NULL,
    standard_indicator character varying(200),
    category_group character varying(200),
    category character varying(200),
    updated_at timestamp without time zone,
    unit character varying,
    deleted boolean not null default false,
    create_time timestamp with time zone not null default CURRENT_TIMESTAMP,
    update_time timestamp with time zone not null default CURRENT_TIMESTAMP
);

--  Add embedding_gemini field for Gemini 1024-dimension vector search (used by indicator_service_v3)
ALTER TABLE th_series_dim ADD COLUMN IF NOT EXISTS embedding_gemini vector(1024);
COMMENT ON COLUMN th_series_dim.embedding_gemini IS 'Gemini embedding (1024 dimensions) for semantic search';

CREATE INDEX IF NOT EXISTS idx_th_series_dim_embedding_gemini
    ON th_series_dim USING hnsw (embedding_gemini vector_cosine_ops);

--  Add embedding_qwen field for Qwen 1024-dimension vector search (selected via DIM_EMBEDDING_PROVIDER=qwen)
ALTER TABLE th_series_dim ADD COLUMN IF NOT EXISTS embedding_qwen vector(1024);
COMMENT ON COLUMN th_series_dim.embedding_qwen IS 'Qwen embedding (1024 dimensions) for semantic search';

CREATE INDEX IF NOT EXISTS idx_th_series_dim_embedding_qwen
    ON th_series_dim USING hnsw (embedding_qwen vector_cosine_ops);

ALTER TABLE th_series_dim ADD COLUMN IF NOT EXISTS department text NULL;