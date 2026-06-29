-- Care circles. A user creates a care circle and invites others into it; once an
-- invitee accepts, every accepted member is mutually in the circle -- the
-- relationship is the shared circle, not a per-pair link, so A<->B and B<->C are
-- the same first-class fact (do two users share a circle?) with no owner-centric
-- asymmetry. Members can then share individual conversations with one another,
-- read-only. A "care circle" is whoever you trust with your health -- family, a
-- partner, a caregiver -- not strictly blood family. These tables stay
-- orthogonal to the single-user scoping the rest of the schema relies on:
-- everything else stays scoped by user_id, and sharing is purely additive,
-- checked at the read paths (history list / conversation fetch). Modern backends
-- only; the legacy backend (res/sql/pg_legacy) has no care-circle feature.

-- A care circle. It is a real object (its owner created it), so it can carry a
-- name and survive membership churn. No FK to users -- consistent with
-- conversations/messages, which also carry a bare user_id (writes all go through
-- the C++ server).
CREATE TABLE IF NOT EXISTS care_circles (
    id             BIGINT       GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    owner_user_id  BIGINT       NOT NULL,             -- creator / admin
    name           VARCHAR(256) NOT NULL DEFAULT '',  -- display name, e.g. "My circle"
    created_at     BIGINT       NOT NULL,             -- unix milliseconds (app-stamped)
    updated_at     BIGINT,                            -- unix milliseconds; set on update, NULL until first
    deleted_at     BIGINT                             -- soft delete: NULL = active, unix milliseconds = when deleted
);

CREATE INDEX IF NOT EXISTS idx_care_circles_owner ON care_circles (owner_user_id);

------------------------------------------------------------------------------

-- Membership: one row per (circle, user). The circle is the set of its Accepted
-- members, so any two of them are in the circle together; the owner is simply
-- the member with role Owner (its membership is created Accepted). An invitee is
-- resolved to a user_id at invite time (created on the fly if they have not
-- signed up yet, mirroring add_or_get_user), so the membership works the moment
-- they first sign in with that email; member_email is kept for display while
-- Pending.
CREATE TABLE IF NOT EXISTS care_circle_members (
    id              BIGINT       GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    care_circle_id  BIGINT       NOT NULL,            -- the circle this membership belongs to
    user_id         BIGINT       NOT NULL,            -- the member (resolved at invite time)
    role            SMALLINT     NOT NULL,            -- database::CircleRole (0=Member 1=Maintainer 2=Owner; >=1 = admin)
    status          SMALLINT     NOT NULL,            -- database::CircleStatus (1=Pending 2=Accepted 3=Declined)
    member_email    VARCHAR(256),                     -- invitee email, for display while Pending
    invite_token    VARCHAR(64),                      -- random token carried by the invite link
    nickname        VARCHAR(256),                     -- admin's display label for this member in the circle (e.g. "Mom"); optional
    health_access   SMALLINT     NOT NULL DEFAULT 0,  -- database::ShareAccess: 0=off, 1=View (members may read), 2=Edit (members may read+write) this member's health (FHIR) data; default off, member-controlled
    created_at      BIGINT       NOT NULL,            -- unix milliseconds (app-stamped)
    updated_at      BIGINT,                           -- unix milliseconds; set on update, NULL until first
    deleted_at      BIGINT                            -- soft delete: NULL = active, unix milliseconds = when deleted
);

CREATE INDEX IF NOT EXISTS idx_care_circle_members_circle ON care_circle_members (care_circle_id);
CREATE INDEX IF NOT EXISTS idx_care_circle_members_user   ON care_circle_members (user_id);
-- One active membership per (circle, user): a re-invite after soft delete starts fresh.
CREATE UNIQUE INDEX IF NOT EXISTS uq_care_circle_members_active
    ON care_circle_members (care_circle_id, user_id) WHERE deleted_at IS NULL;

------------------------------------------------------------------------------

-- Per-conversation share grant: a member shares one conversation with one fellow
-- circle member, read-only. The read paths union these in on top of the owner's
-- own user_id scoping; "remove from my history" soft-deletes the grant, never
-- the owner's conversation.
CREATE TABLE IF NOT EXISTS conversation_shares (
    id                  BIGINT     GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    conversation_id     BIGINT     NOT NULL,          -- = conversations.id (the opening question's id)
    owner_user_id       BIGINT     NOT NULL,          -- who shared (the conversation owner)
    shared_with_user_id BIGINT     NOT NULL,          -- the circle member granted access
    access_level        SMALLINT   NOT NULL,          -- database::ShareAccess (1=View)
    created_at          BIGINT     NOT NULL,          -- unix milliseconds (app-stamped)
    deleted_at          BIGINT                        -- soft delete: NULL = active, unix milliseconds = when deleted
);

CREATE INDEX IF NOT EXISTS idx_conversation_shares_conv   ON conversation_shares (conversation_id);
CREATE INDEX IF NOT EXISTS idx_conversation_shares_member ON conversation_shares (shared_with_user_id);
-- One active grant per (conversation, member): a re-share after unshare starts fresh.
CREATE UNIQUE INDEX IF NOT EXISTS uq_conversation_shares_active
    ON conversation_shares (conversation_id, shared_with_user_id) WHERE deleted_at IS NULL;
