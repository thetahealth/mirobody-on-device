# `src/circle` — care circles

A **care circle** is whoever you trust with your health — family, a partner, a
caregiver. The module owns the social graph and two kinds of sharing layered on
top of the otherwise strictly single-user core:

- **Conversation sharing** — grant a fellow member read/edit access to one chat
  conversation; it then shows up in their history.
- **Health-data sharing** — a per-member switch that lets the other accepted
  members of a circle read (or read+write) your FHIR records, so the AI can
  answer "how is my family doing?".

Everything here is **additive and checked at the read/write paths** — no other
table is rescoped. The schema lives in `res/sql/sqlite/2_care_circle.sql`.

On a phone the care circle is read-only context: the circle itself (members,
invitations, shares) lives on the mirobody server the app is connected to. This
service is what the loopback front door still answers until the apps talk to that
server for it.

## Model

A circle is a **group**, not a per-pair link: any two *Accepted* members are
mutually in the circle, so "do X and Y share a circle?" is one symmetric
self-join on `care_circle_id`, with no owner-centric asymmetry. A user may own
or belong to **any number** of circles.

Three tables (see [`res/sql/sqlite/2_care_circle.sql`](../../res/sql/sqlite/2_care_circle.sql)):

- **`care_circles`** — the group object (owner, name, lifecycle). Survives
  membership churn.
- **`care_circle_members`** — one row per (circle, user): `role`, `status`,
  `member_email` / `invite_token` (pending invites), `nickname` (an admin's
  display label, e.g. "Mom"), and `health_access` (this member's sharing level).
- **`conversation_shares`** — one read/edit grant of a conversation to one member.

### Roles & status

`role` is `database::CircleRole` (privilege-ordered, so `>= Maintainer` ⇒ admin):

| Role | Value | Can |
| ---- | ----- | --- |
| Member     | 0 | view the roster; set their **own** health-sharing level |
| Maintainer | 1 | + invite / remove **Members**, set nicknames |
| Owner      | 2 | + remove maintainers, rename/delete the circle, change roles |

`status` is `database::CircleStatus` — `Pending` (1) on invite, `Accepted` (2)
once the invitee joins, `Declined` (3). Only **Accepted** members count for the
co-membership joins. The owner's own membership is created `Owner` + `Accepted`.
Guardrails: nobody removes the owner; a maintainer removes only plain Members;
roles are owner-only and can't target the owner or mint a second owner.

### Health access

`health_access` is `database::ShareAccess` — `0` off / `1` View (members may
read your FHIR data) / `2` Edit (members may read **and write** it). Default off,
each member controls their **own** value, **per circle** (you can share with
Family but not Work).

[`access.hpp`](access.hpp) is the tiny seam the FHIR layer depends on without
pulling in the whole service:

- `can_read_health(db, viewer, target)` — true for self, or co-Accepted in a
  shared circle where `target.health_access >= View`.
- `can_write_health(db, viewer, target)` — same, but requires `Edit`.
- `resolve_health_subject(db, viewer, member, need_write)` — maps an **opaque
  member handle** (`care_circle_members.id`) to the underlying target user id,
  but only when `viewer` is authorized through that handle's own circle; `0`
  otherwise. The opaque-id entry point so a raw users PK never crosses the wire.

[`src/fhir/rest.cpp`](../fhir/rest.cpp) calls `resolve_health_subject` from
`resolve_read_subject` / `resolve_write_subject`: a request may target another
user's records with `?subject=<member>` (the opaque handle, **not** a user id)
only when authorized, else a `403` OperationOutcome. Writes without `?subject=`
stay self-only.

### Opaque member handles

The care-circle and FHIR/chat surfaces never expose the global `users` primary
key. Every cross-user reference — circle member lists, the "shared with me"
picker, conversation-share targets, the FHIR/chat `subject` — is a
`care_circle_members.id` **handle** (`member` in JSON), which the server resolves
back to a user id with an access check. The caller's *own* id is only ever taken
from the authenticated JWT (`req.user_id`), so a client neither sends nor
receives a raw users PK. (`HealthShare::user_id` is retained for in-process
gating only and is never serialized.)

## HTTP routes

All JWT-guarded (`require_auth`), under `HTTP_URI_PREFIX` when set.

| Method | Path | Purpose |
| ------ | ---- | ------- |
| POST | `/api/circle/create`   | create a circle `{name}` (you become its Owner) |
| POST | `/api/circle/rename`   | rename `{care_circle_id, name}` — owner only |
| POST | `/api/circle/delete`   | soft-delete the circle + its memberships — owner only |
| POST | `/api/circle/invite`   | invite `{email, care_circle_id?}` — admin; emails a join link (best-effort); rate-limited per inviter |
| GET\|POST | `/api/circle/members` | every circle I belong to (members carry an opaque `member` handle, never a user id) + invites to me |
| POST | `/api/circle/accept`   | accept `{token}` |
| POST | `/api/circle/decline`  | decline `{token}` |
| POST | `/api/circle/remove`   | remove `{member}` — admin (see guardrails) |
| POST | `/api/circle/nickname` | set/clear `{member, nickname}` — admin |
| POST | `/api/circle/role`     | set `{member, role}` to Member/Maintainer — owner only |
| POST | `/api/circle/health-sharing` | set my level `{access: "off"\|"view"\|"edit", care_circle_id}` — per circle; `care_circle_id` required, caller must be a member |
| POST | `/api/conversation/share`   | `{conversation_id, members[], access}` — to Accepted co-members |
| POST | `/api/conversation/unshare` | `{conversation_id, member}` |
| GET\|POST | `/api/conversation/shares` | `?id=` current recipients (by email) of a conversation |

The `member` / `members` fields are opaque `care_circle_members.id` handles (the
`member` value returned by `/api/circle/members` and `/api/circle/health-shared-with-me`),
resolved server-side; `remove` / `nickname` / `role` derive the circle from the
handle, so they no longer need `care_circle_id`.

Responses use the standard `{code, msg, data}` envelope. The matching reads —
listing conversations shared *to* you and fetching a shared thread read-only —
live in `chat::ChatService` (`GET /api/conversation`, the extended
`/api/history`), since they sit on the chat schema.

## Limits

Caps from `CircleConfig` (`src/config`). The **Shipped default** column is what
the committed `config.example.yml` sets — and since that file is the lowest
config layer, those are the effective defaults unless overridden. (The two size
caps compile to `0` = unlimited; the invite cap compiles to `20` / `3600s`.)

| Config key | Shipped default | Enforced in | Effect |
| ---------- | --------------- | ----------- | ------ |
| `CIRCLE_MAX_PER_USER`      | `5` | `handle_create`  | rejects a new circle once you own this many (`-4`) |
| `CIRCLE_MAX_MEMBERS`       | `5` | `handle_invite`  | rejects a new invite once the circle has this many active members — pending invites included (`-9`) |
| `CIRCLE_INVITE_MAX` / `CIRCLE_INVITE_WINDOW_SEC` | `10` / `1800s` | `handle_invite` | per-inviter invitations per rolling window, counted in the cache *before* a shell account is created or mail is sent (`-8`) |

Set any to `0` to disable that cap. A re-invite of an existing pending/declined
member reuses its row, so it doesn't count against `CIRCLE_MAX_MEMBERS`.

## Files

- **`service.{hpp,cpp}`** — `CircleService`: registers the routes and owns the
  graph + sharing logic. Helpers: `create_circle` / `ensure_owned_circle`,
  `owns_circle` (owner-only gate), `member_role` / `can_admin_circle`
  (invite/remove gate), `in_circle_together` (sharing gate),
  `resolve_or_create_user` (email → user id, mirroring `add_or_get_user`),
  `revoke_shares_between`, `random_token`, `send_invite_email`.
- **`access.hpp` / `access.cpp`** — `can_read_health` / `can_write_health`, the
  FHIR-facing authorization seam (impl lives in `service.cpp`).

Constructed in [`src/server/server.cpp`](../server/server.cpp) alongside the
other services; enums in
[`src/database/enums.hpp`](../database/enums.hpp).
