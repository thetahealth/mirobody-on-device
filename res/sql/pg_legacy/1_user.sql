CREATE TABLE IF NOT EXISTS health_app_user (
    id            INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    create_at     TIMESTAMP WITHOUT TIME ZONE NOT NULL DEFAULT CURRENT_TIMESTAMP,
    update_at     TIMESTAMP WITHOUT TIME ZONE NOT NULL DEFAULT CURRENT_TIMESTAMP,
    is_del        BOOLEAN NOT NULL,
    email         CHARACTER VARYING NOT NULL UNIQUE,
    name          CHARACTER VARYING NOT NULL DEFAULT ''::CHARACTER VARYING,
    consultant_id INTEGER NOT NULL DEFAULT 0,
    lang          CHARACTER VARYING NOT NULL DEFAULT 'en'::CHARACTER VARYING,
    apple_sub     VARCHAR(255),
    response_lang CHARACTER VARYING(64) DEFAULT NULL::CHARACTER VARYING,
    gender        INTEGER,
    birth         CHARACTER VARYING,
    blood         CHARACTER VARYING,
    tz            CHARACTER VARYING NOT NULL DEFAULT ''::CHARACTER VARYING,
    coins         INTEGER DEFAULT 0
);

ALTER TABLE health_app_user ADD COLUMN IF NOT EXISTS wechat_openid VARCHAR(64);
ALTER TABLE health_app_user ADD COLUMN IF NOT EXISTS ethnicity VARCHAR(128);
ALTER TABLE health_app_user ADD COLUMN IF NOT EXISTS mfa_enabled BOOLEAN NOT NULL DEFAULT FALSE;

CREATE INDEX IF NOT EXISTS idx_health_app_user_wechat_openid ON health_app_user USING btree (wechat_openid);
CREATE INDEX IF NOT EXISTS idx_health_app_user_apple_sub ON health_app_user USING btree (apple_sub);

CREATE UNIQUE INDEX IF NOT EXISTS idx_uni_health_app_user_email_active ON health_app_user USING btree (email) WHERE (is_del = false);
CREATE UNIQUE INDEX IF NOT EXISTS idx_uni_health_app_user_apple_sub_active ON health_app_user USING btree (apple_sub) WHERE (is_del = false);

COMMENT ON COLUMN health_app_user.gender IS 'Gender: 0-Unknown 1-Male 2-Female';
