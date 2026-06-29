-- Care circles. SQLite port of res/sql/pg/2_care_circle.sql -- same model (a care
-- circle is a group; any two accepted members are mutually in the circle),
-- dialect differences: INTEGER PRIMARY KEY (rowid) ids, INTEGER user ids and
-- timestamps (unix milliseconds, app-stamped), TEXT for strings. Partial unique
-- indexes (WHERE deleted_at IS NULL) are supported natively. See
-- pg/2_care_circle.sql for the model. Modern backends only; the legacy backend
-- has no care-circle feature.
CREATE TABLE IF NOT EXISTS care_circles (
    id             INTEGER NOT NULL PRIMARY KEY,             -- rowid alias, auto-assigned
    owner_user_id  INTEGER NOT NULL,                         -- creator / admin
    name           TEXT    NOT NULL DEFAULT '',              -- display name, e.g. "My circle"
    created_at     INTEGER NOT NULL,                         -- unix milliseconds (app-stamped)
    updated_at     INTEGER,                                  -- unix milliseconds; set on update, NULL until first
    deleted_at     INTEGER                                   -- soft delete: NULL = active, unix milliseconds = when deleted
);

CREATE INDEX IF NOT EXISTS idx_care_circles_owner ON care_circles (owner_user_id);

------------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS care_circle_members (
    id              INTEGER NOT NULL PRIMARY KEY,            -- rowid alias, auto-assigned
    care_circle_id  INTEGER NOT NULL,                        -- the circle this membership belongs to
    user_id         INTEGER NOT NULL,                        -- the member (resolved at invite time)
    role            INTEGER NOT NULL,                        -- database::CircleRole (0=Member 1=Maintainer 2=Owner; >=1 = admin)
    status          INTEGER NOT NULL,                        -- database::CircleStatus (1=Pending 2=Accepted 3=Declined)
    member_email    TEXT,                                    -- invitee email, for display while Pending
    invite_token    TEXT,                                    -- random token carried by the invite link
    nickname        TEXT,                                    -- admin's display label for this member in the circle (e.g. "Mom"); optional
    health_access   INTEGER NOT NULL DEFAULT 0,              -- database::ShareAccess: 0=off, 1=View (read), 2=Edit (read+write) this member's health (FHIR) data; default off, member-controlled
    created_at      INTEGER NOT NULL,                        -- unix milliseconds (app-stamped)
    updated_at      INTEGER,                                 -- unix milliseconds; set on update, NULL until first
    deleted_at      INTEGER                                  -- soft delete: NULL = active, unix milliseconds = when deleted
);

CREATE INDEX IF NOT EXISTS idx_care_circle_members_circle ON care_circle_members (care_circle_id);
CREATE INDEX IF NOT EXISTS idx_care_circle_members_user   ON care_circle_members (user_id);
-- One active membership per (circle, user): a re-invite after soft delete starts fresh.
CREATE UNIQUE INDEX IF NOT EXISTS uq_care_circle_members_active
    ON care_circle_members (care_circle_id, user_id) WHERE deleted_at IS NULL;

------------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS conversation_shares (
    id                  INTEGER NOT NULL PRIMARY KEY,         -- rowid alias, auto-assigned
    conversation_id     INTEGER NOT NULL,                     -- = conversations.id (the opening question's id)
    owner_user_id       INTEGER NOT NULL,                     -- who shared (the conversation owner)
    shared_with_user_id INTEGER NOT NULL,                     -- the circle member granted access
    access_level        INTEGER NOT NULL,                     -- database::ShareAccess (1=View)
    created_at          INTEGER NOT NULL,                     -- unix milliseconds (app-stamped)
    deleted_at          INTEGER                               -- soft delete: NULL = active, unix milliseconds = when deleted
);

CREATE INDEX IF NOT EXISTS idx_conversation_shares_conv   ON conversation_shares (conversation_id);
CREATE INDEX IF NOT EXISTS idx_conversation_shares_member ON conversation_shares (shared_with_user_id);
-- One active grant per (conversation, member): a re-share after unshare starts fresh.
CREATE UNIQUE INDEX IF NOT EXISTS uq_conversation_shares_active
    ON conversation_shares (conversation_id, shared_with_user_id) WHERE deleted_at IS NULL;
