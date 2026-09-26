# Database

`mirobody_core` keeps its persistence behind one `Database` class
([database.hpp](database.hpp)) over **SQLite** ([sqlite.cpp](sqlite.cpp)), the
only backend this repo links: a phone holds one person's record, the host app is
the only writer, and one file is what the platform's backup, export and delete
all work on. The development build uses the same backend and schema, so what the
tests cover is what ships. The server-side databases (Postgres and friends) belong
to the [main mirobody repo](https://github.com/thetahealth/mirobody).

The build defines `MIROBODY_DATABASE_SQLITE` and `MIROBODY_DATABASE_BACKEND_DIR`
(`"sqlite"`, the `res/sql/` subdirectory the schema loader reads).

## Configuring

```yaml
# config.yml -- a real file, not :memory:: migration and the core open
# separate connections, so an in-memory DB would not share its schema.
SQLITE_PATH: '_local/mirobody.db'
DB_INIT_SCHEMA: true
```

The phone apps pass the path through the C ABI instead
(`mirobody_start(config, data_dir)` puts the file at `<data_dir>/mirobody.db`).

## Schema file layout

The DDL lives under [`res/sql/sqlite/`](../../res/sql/sqlite) and is applied in
filename order at first boot (`apply_schema` reads every `*.sql` in the
directory, lexicographically). Files carry a single-digit prefix that groups
them by the kind of object they define, so load order is right by construction:

| Prefix    | Holds                                                  | Current files |
| --------- | ------------------------------------------------------ | ------------- |
| `1_*.sql` | Entities — tables, with their indexes / inline FKs     | `1_user.sql`, `1_chat.sql`, `1_data.sql`, `1_health.sql`, `1_memory.sql` |
| `2_*.sql` | Relations — cross-entity foreign keys / junction tables | `2_care_circle.sql` |

Within a prefix, files load alphabetically. The entity files are self-contained
— each file's foreign keys point only at tables defined earlier in the *same*
file (e.g. `user_identities` → `users` in `1_user.sql`) — so the order among the
`1_*` files does not matter.

This schema mirrors the tables of the old server it was ported from. It is not
the long-term contract: the record's portable form is the FHIR Bundle export the
main repo defines, and this schema will move toward what that export needs.

## User accounts and login

The user-domain schema lives in `res/sql/sqlite/1_user.sql`:

- `users` — one row per account. `email` is nullable, because a provider-only
  signup (Apple Private Relay, or X with no email) may not have one. Among
  non-deleted rows email is unique (`idx_uni_users_email_active`), so
  re-registration after a soft delete is allowed. Soft delete is the `deleted_at`
  timestamp (`NULL` = active).
- `user_identities` — the external (OAuth) accounts linked to a user. A user may
  link many. `(login_method, provider_uid)` is globally unique, so one external
  account maps to at most one user.
- `user_login_logs` — append-only history, one row per login.
- `user_mcp_access_logs` — append-only history, one row per MCP
  tool / resource / prompt / skill access.

`login_method` is the `LoginMethod` enum from [`enums.hpp`](enums.hpp), stored as
a small integer rather than a lookup table; `kind` in the MCP log is `McpKind`
from the same header. Both columns are bare `SMALLINT` — the C++ server is the
only writer, so the enum is the single source of truth for valid values.

### Login resolution

- **Email / magic-link** resolves directly against `users.email`: send the link
  to the address, then match the verified email back to the active row.
- **OAuth** (Apple, Google, GitHub, X, WeChat) resolves through
  `user_identities`: look up `(login_method, provider_uid)` — a hit is the
  existing user, a miss is a new identity.

Linking is **explicit only**. A logged-in user adds another method from account
settings, which inserts a new `user_identities` row. Accounts are never merged
automatically on a matching email, because an OAuth provider may report an
unverified address and silent linking would let an attacker hijack an account.

A provider-only account (no `users.email`, e.g. an X or Apple sign-in that
withheld it) can **bind an email** while logged in: `POST /email/bind` sends a
code, `POST /email/bind/verify` checks it and sets `users.email`. Binding
proves ownership via the code, and rejects an address already held by another
active account (no auto-merge) — so the bound email is safe to use for
magic-link login afterwards.

## Merging user accounts

> **TODO — not implemented yet.** There is no `merge_users()` routine and no
> endpoint that merges accounts; the section below is the *intended design*, not
> shipped behavior. Today the duplicate-account cases are simply prevented or
> rejected: third-party logins never auto-merge on email, and binding an email
> (`/email/bind`) rejects an address already in use rather than merging. Build
> the routine below when account merging is actually needed.

When one person ends up with two accounts (for example an email signup and a
later OAuth signup that was not auto-linked), the intended fix is to merge them
by keeping one row and reassigning everything that points at the other — always
an explicit, deliberate operation.

Pick a **surviving** id to keep and a **merged** id to retire, then run the whole
thing in one transaction. The example merges user `25` into user `10` (the
snippet is PostgreSQL syntax; the shape is the same on the other backends, but
`UPDATE ... FROM` and the timestamp function vary by dialect):

```sql
BEGIN;

-- Lock both rows so nothing writes to them mid-merge.
SELECT id FROM users WHERE id IN (10, 25) FOR UPDATE;

-- Backfill any field the surviving row is missing (here, email).
UPDATE users AS keep
SET email = merged.email
FROM users AS merged
WHERE keep.id = 10 AND merged.id = 25
  AND keep.email IS NULL AND merged.email IS NOT NULL;

-- Reassign every child row from the merged id to the surviving id.
UPDATE user_identities      SET user_id = 10 WHERE user_id = 25;
UPDATE user_login_logs      SET user_id = 10 WHERE user_id = 25;
UPDATE user_mcp_access_logs SET user_id = 10 WHERE user_id = 25;

-- Retire the merged account (soft delete keeps an audit trail). deleted_at is
-- unix milliseconds the app stamps (now_unix_ms); a literal stands in here.
UPDATE users SET deleted_at = 1718000000000 WHERE id = 25;

COMMIT;
```

Things to get right:

- **Order matters — reassign first, retire last.** Every child table is
  `ON DELETE CASCADE`, so a hard `DELETE FROM users` on the merged id would take
  its rows with it. Soft-deleting (as above) never triggers the cascade; if you
  prefer a hard delete, reassign the children first so the row has no dependents
  left.
- **The active-email unique index does not collide.**
  `idx_uni_users_email_active` only covers rows with `deleted_at IS NULL`, so
  once the merged row is soft-deleted its email no longer competes. Backfill the
  surviving email before retiring the other row, as shown.
- **Identities reassign safely.** `user_identities` is unique on
  `(login_method, provider_uid)` globally, so moving `user_id` can never violate
  it. After the merge the surviving user may hold two identities of the same
  method (e.g. two Google accounts); that is allowed by design.

## Chat history

The chat-domain schema lives in `res/sql/sqlite/1_chat.sql`:

> **Timestamp convention.** Across the modern schema (`conversations`, `messages`,
> `files`, `users` + its log tables, `memories`) every `created_at` / `updated_at`
> / `deleted_at` is **unix milliseconds** in a `BIGINT` / `INTEGER` column, stamped
> by the app (`platform::now_unix_ms`) — no DB `DEFAULT CURRENT_TIMESTAMP` or
> `updated_at` triggers. `created_at` is `NOT NULL`; `updated_at` and `deleted_at`
> are NULL until the first update / soft-delete. Milliseconds because wearable
> health data is ms-native, so one scale avoids unit conversions.

- `conversations` — a **thin** per-conversation header: just `id`, `user_id`,
  `summary`, `created_at`, and a `deleted_at` soft-delete column, read back by
  `GET /api/history` / `POST /api/history/delete`. The modern `id` is **not
  auto-assigned** — it equals the conversation's opening question's `messages.id`,
  so a single id names the thread across both tables.
- `messages` — a per-message log, **one row per message, not per turn**: a single
  user question fans out to one response row per answering agent (agents may
  respond simultaneously), so a turn is a question row plus N response rows that
  point back at it via `question_id`. Columns: `id` (per-message key),
  `user_id`, `conversation_id` (the thread = the opening question's id; NULL on the
  opening question itself, which is the root), `question_id` (NULL on question rows;
  the answered question on response rows), `role`, `agent` / `provider` (which
  agent and LLM provider produced a response; NULL for questions), `content`,
  the per-question `language` / `timezone`
  / `files` (NULL on response rows), `created_at`, `deleted_at`. Indexed by
  `(conversation_id, created_at)`, `(question_id)`, and `(user_id)`.
  `Chat::persist_history` writes the **opening question** row (and seeds
  the matching `conversations` row with its id) in one transaction; agent
  **responses** are not persisted yet, and the live multi-turn window is still
  cache-backed ([`../chat/history.hpp`](../chat/history.hpp); in-process).
- `files` — the per-user uploads index behind `GET /api/files`, documented in
  [`../chat/README.md`](../chat/README.md).

`messages.role` is the `MessageRole` enum from [`enums.hpp`](enums.hpp), stored as
a `SMALLINT` — the same single-source-of-truth convention as `LoginMethod` /
`McpKind` above.
- **Reassign *every* table that references `users(id)`.** The list above is just
  the user-domain set; a real merge must move all other rows keyed by user too.
  When this is built, centralize it in a single `merge_users(keep_id, merged_id)`
  routine so a newly added table is updated in exactly one place and never
  silently left behind.
