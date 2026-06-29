-- Care circles. MySQL port of res/sql/pg/2_care_circle.sql -- same model (a care
-- circle is a group; any two accepted members are mutually in the circle),
-- dialect differences: AUTO_INCREMENT ids, INT user ids (matching users.id / the
-- chat tables), BIGINT unix-millisecond timestamps (app-stamped), inline
-- KEY/UNIQUE KEY (no CREATE INDEX IF NOT EXISTS), and functional CASE indexes for
-- "unique among active rows" (no partial indexes). See pg/2_care_circle.sql for
-- the model. Modern backends only; the legacy backend has no care-circle feature.
CREATE TABLE IF NOT EXISTS care_circles (
    id             BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
    owner_user_id  INT          NOT NULL,             -- creator / admin
    name           VARCHAR(256) NOT NULL DEFAULT '',  -- display name, e.g. "My circle"
    created_at     BIGINT       NOT NULL,             -- unix milliseconds (app-stamped)
    updated_at     BIGINT,                            -- unix milliseconds; set on update, NULL until first
    deleted_at     BIGINT,                            -- soft delete: NULL = active, unix milliseconds = when deleted
    KEY idx_care_circles_owner (owner_user_id)
);

------------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS care_circle_members (
    id              BIGINT       NOT NULL AUTO_INCREMENT PRIMARY KEY,
    care_circle_id  BIGINT       NOT NULL,            -- the circle this membership belongs to
    user_id         INT          NOT NULL,            -- the member (resolved at invite time)
    role            SMALLINT     NOT NULL,            -- database::CircleRole (0=Member 1=Maintainer 2=Owner; >=1 = admin)
    status          SMALLINT     NOT NULL,            -- database::CircleStatus (1=Pending 2=Accepted 3=Declined)
    member_email    VARCHAR(256),                     -- invitee email, for display while Pending
    invite_token    VARCHAR(64),                      -- random token carried by the invite link
    nickname        VARCHAR(256),                     -- admin's display label for this member in the circle (e.g. "Mom"); optional
    health_access   TINYINT      NOT NULL DEFAULT 0,  -- database::ShareAccess: 0=off, 1=View (read), 2=Edit (read+write) this member's health (FHIR) data; default off, member-controlled
    created_at      BIGINT       NOT NULL,            -- unix milliseconds (app-stamped)
    updated_at      BIGINT,                           -- unix milliseconds; set on update, NULL until first
    deleted_at      BIGINT,                           -- soft delete: NULL = active, unix milliseconds = when deleted
    KEY idx_care_circle_members_circle (care_circle_id),
    KEY idx_care_circle_members_user   (user_id),
    -- One active membership per (circle, user): functional index keys active rows
    -- by (circle, user) and soft-deleted rows by NULL (NULLs not unique).
    UNIQUE KEY uq_care_circle_members_active (
        (CASE WHEN deleted_at IS NULL THEN care_circle_id END),
        (CASE WHEN deleted_at IS NULL THEN user_id END))
);

------------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS conversation_shares (
    id                  BIGINT     NOT NULL AUTO_INCREMENT PRIMARY KEY,
    conversation_id     BIGINT     NOT NULL,          -- = conversations.id (the opening question's id)
    owner_user_id       INT        NOT NULL,          -- who shared (the conversation owner)
    shared_with_user_id INT        NOT NULL,          -- the circle member granted access
    access_level        SMALLINT   NOT NULL,          -- database::ShareAccess (1=View)
    created_at          BIGINT     NOT NULL,          -- unix milliseconds (app-stamped)
    deleted_at          BIGINT,                       -- soft delete: NULL = active, unix milliseconds = when deleted
    KEY idx_conversation_shares_conv   (conversation_id),
    KEY idx_conversation_shares_member (shared_with_user_id),
    UNIQUE KEY uq_conversation_shares_active (
        (CASE WHEN deleted_at IS NULL THEN conversation_id END),
        (CASE WHEN deleted_at IS NULL THEN shared_with_user_id END))
);
