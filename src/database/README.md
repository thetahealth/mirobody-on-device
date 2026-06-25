# Database backends

`mirobody_core` keeps its in-process SQL persistence behind a single
`Database` class and links exactly one concrete backend at build time,
selected via the `MIROBODY_DATABASE_BACKEND` CMake option.

`MIROBODY_DATABASE_BACKEND` takes one of these values:

| Value               | Client (links)       | DDL loaded from     |
| ------------------- | -------------------- | ------------------- |
| `SQLITE`            | SQLite3              | `res/sql/sqlite`    |
| `DUCKDB`            | DuckDB               | `res/sql/duckdb`    |
| `POSTGRESQL`        | libpq                | `res/sql/pg`        |
| `POSTGRESQL_LEGACY` | libpq (same as `POSTGRESQL`) | `res/sql/pg_legacy` |
| `MYSQL`             | libmysql             | `res/sql/mysql`     |
| `CLICKHOUSE`        | (stub, none yet)     | `res/sql/clickhouse`|

| Build target                             | Default             | Allowed                                           |
| ---------------------------------------- | ------------------- | ------------------------------------------------- |
| Mobile (Android, iOS)                    | `SQLITE`            | `SQLITE`, `DUCKDB`                                 |
| Desktop / server (Windows, Linux, macOS) | `POSTGRESQL`        | any of the above                                  |

On mobile the backend is restricted to `SQLITE` or `DUCKDB` because each device
holds a single user's data and the host app is the only writer. Desktop and
server deployments default to `POSTGRESQL` (the libpq client loading the modern
`res/sql/pg` DDL); `POSTGRESQL_LEGACY` is the same client but loads
`res/sql/pg_legacy` instead. Any backend is permitted, so `SQLITE` remains
available there for single-writer or local/test setups.

Each value defines a preprocessor macro named after its DDL subdirectory,
uppercased — `MIROBODY_DATABASE_PG`, `MIROBODY_DATABASE_PG_LEGACY`,
`MIROBODY_DATABASE_SQLITE`, `_DUCKDB`, `_MYSQL`, `_CLICKHOUSE` — plus the string
`MIROBODY_DATABASE_BACKEND_DIR` (that same subdirectory). Code that is common to
the two PostgreSQL variants guards on
`MIROBODY_DATABASE_PG || MIROBODY_DATABASE_PG_LEGACY` (see `main.cpp` /
`server/server.cpp`).

## Selecting and configuring a backend

It's **two steps**: choose the backend at **build** time, then set its connection
keys at **run** time (`config.yml`). Each backend builds into its own directory
(`build`, `build_legacy`, …), so switching backends is just a different build token —
the previously built directory stays configured and reusable.

### Step 1 — build (pick the backend)

Use the **`build.cmd` / `build.sh` wrappers** — they set up the compiler
environment (and, on Windows, the vcpkg toolchain) and Ninja for you (a bare
`cmake -B build` skips all that and won't find the dependencies). The backend is
a **command-line token** (no env var); pass it in any order alongside the arch
and `clean`:

```sh
# Linux / macOS
./build.sh             # POSTGRESQL (default) -> build
./build.sh legacy      # POSTGRESQL_LEGACY    -> build_legacy
./build.sh sqlite      # SQLITE              -> build_sqlite
```
```cmd
:: Windows
build.cmd pg
build.cmd sqlite
build.cmd
```

Tokens: `pg` / `postgresql`, `legacy` / `pg_legacy`, `mysql`, `sqlite`, `duckdb`,
`ck` / `clickhouse`. Each backend (and arch) builds into its own directory —
`build[_<arch>][_<backend>]`: the plain host + `POSTGRESQL` build is just
`build`, `legacy` → `build_legacy`, and so on. Because they don't share a directory you
can keep several configured at once and switch by re-running with a different
token — no reconfigure. Pass `clean` (e.g. `build.cmd pg clean`) only to force
that one directory to reconfigure from scratch.

### Step 2 — configure the connection (`config.yml`)

Set the keys for the backend you built (the rest are ignored):

```yaml
# PostgreSQL
PG_HOST: 127.0.0.1
PG_PORT: 5432
PG_USER: postgres
PG_PASSWORD: secret
PG_DBNAME: mirobody

# …or SQLite — use a real file, not :memory: (migration and the server use
# separate connections, so an in-memory DB would not share its schema).
SQLITE_PATH: mirobody.sqlite
```

DuckDB / MySQL / ClickHouse follow the same shape — swap the backend value and
set that backend's keys (`DUCKDB_PATH`, `MYSQL_*`, `CLICKHOUSE_*`); see the
inline docs in [config.yml](../../config.yml).

Confirm what a binary was built with (use the directory for the backend you
built, e.g. `build`, `build_legacy`):

```sh
grep MIROBODY_DATABASE_BACKEND build*/CMakeCache.txt      # findstr on Windows
```

> **First boot creates the tables** via an idempotent schema migration — but it
> is **skipped unless `ENV` is set to a non-managed value** (anything other than
> `test` / `gray` / `prod`; an *unset* `ENV` also skips). For a fresh local DB,
> run with e.g. `ENV=local`. In managed deployments (`ENV=test`/`gray`/`prod`)
> the schema is applied out-of-band from `res/sql/<backend>/`.

## Schema file layout

The DDL for each backend lives under `res/sql/<backend>/` and is applied in
filename order at first boot (`apply_schema` reads every `*.sql` in the
directory, lexicographically). Files carry a single-digit prefix that groups
them by the kind of object they define, so load order is right by construction:

| Prefix    | Holds                                                  | Current files |
| --------- | ------------------------------------------------------ | ------------- |
| `0_*.sql` | DB setup — extensions, functions, triggers             | `0_setup.sql` (pg) |
| `1_*.sql` | Entities — tables, with their indexes / inline FKs     | `1_user.sql`, `1_chat.sql`, `1_health.sql`, `1_memory.sql` |
| `2_*.sql` | Relations — cross-entity foreign keys / junction tables | *(none yet)* |
| `3_*.sql` | Views                                                  | *(none yet)* |

Within a prefix, files load alphabetically. The entity files are self-contained
— each file's foreign keys point only at tables defined earlier in the *same*
file (e.g. `user_identities` → `users` in `1_user.sql`) — so the order among the
`1_*` files does not matter, and the `2_*` tier exists for any future
cross-entity relation that would need to load after all entities.

`res/sql/pg_legacy/` follows the same prefix convention; it differs from
`res/sql/pg/` only in schema *shape* — the table/column names the legacy Python
stack uses (`health_app_user`, `th_sessions`, …) — not in file naming.

## Client libraries

Exactly one of these is linked into `mirobody_core`, picked by
`MIROBODY_DATABASE_BACKEND`. SQLite is the mobile default; `POSTGRESQL`
(the libpq client with the modern `res/sql/pg` schema) is the desktop
default — install only the one(s) you actually plan to build against.

| Package                       | Direct download                                                          |
| ----------------------------- | ------------------------------------------------------------------------ |
| SQLite amalgamation           | https://sqlite.org/download.html                                         |
| PostgreSQL (ships `libpq`)    | https://www.postgresql.org/download/                                     |
| Oracle MySQL Connector/C      | https://dev.mysql.com/downloads/connector/c/                             |
| MariaDB Connector/C (LGPL)    | https://mariadb.com/downloads/connectors/connectors-data-access/c-connector |
| DuckDB C/C++ library          | https://duckdb.org/docs/installation/                                    |

SQLite is system-provided on iOS and Android, so the entry above only matters
on desktop builds. Linux / macOS users just `apt install libpq-dev` /
`brew install libpq` (or the equivalent for the backend they picked) — see
"Building — Linux / WSL / macOS" in the [top-level README](../../README.md) for
the full apt / dnf / brew command lines.

## SQL portability

The schema and queries used by the `Database` wrapper stay inside the common
subset of these five dialects. The table below lists the places where they
diverge.

| Feature | SQLite | DuckDB | PostgreSQL | MySQL | ClickHouse |
|---|---|---|---|---|---|
| Type system | dynamic (type affinity) | strict | strict | strict | strict (`Nullable()` opt-in) |
| Auto-increment | `INTEGER PRIMARY KEY [AUTOINCREMENT]` | `BIGINT GENERATED ALWAYS AS IDENTITY` | `BIGINT GENERATED ALWAYS AS IDENTITY` | `BIGINT AUTO_INCREMENT` | no native sequence; `UUID DEFAULT generateUUIDv4()` is idiomatic |
| Timestamps (tz-aware) | `INTEGER` Unix epoch (no native tz type); `datetime(ts, 'unixepoch')`, `strftime()` | `TIMESTAMPTZ`; `EXTRACT`, `DATE_TRUNC`, `+ INTERVAL` | `TIMESTAMPTZ`; same as DuckDB | `TIMESTAMP` (UTC-backed, range 1970-2038); `DATE_ADD(ts, INTERVAL ...)` | `DateTime('UTC')`, `DateTime64(3, 'UTC')`; `toStartOf*`, `dateAdd`, `+ INTERVAL` |
| Boolean | `INTEGER` 0 / 1 | `BOOLEAN` | `BOOLEAN` | `TINYINT(1)` (alias `BOOLEAN`) | `Bool` (alias `UInt8`) |
| String concat | `\|\|` | `\|\|` | `\|\|` | `CONCAT(a, b)` (no `\|\|` without ANSI mode) | `\|\|` or `concat(a, b)` |
| String aggregation | `GROUP_CONCAT(x, ',')` | `STRING_AGG(x, ',')` or `LIST(x)` | `STRING_AGG(x, ',')` | `GROUP_CONCAT(x SEPARATOR ',')` | `arrayStringConcat(groupArray(x), ',')` |
| Upsert | `ON CONFLICT (col) DO UPDATE` (3.24+) | `ON CONFLICT (col) DO UPDATE` | `ON CONFLICT (col) DO UPDATE` | `ON DUPLICATE KEY UPDATE` (different shape) | no synchronous upsert; `ReplacingMergeTree` dedupes asynchronously at merge time |
| `INSERT ... RETURNING` | yes (3.35+) | yes | yes | no (MySQL 8.x; MariaDB has it) | no |
| Regex | `REGEXP` (needs extension) | `REGEXP_REPLACE` built-in | `REGEXP_REPLACE`, `~` operator | `REGEXP_REPLACE` (8.0+) | `match()`, `replaceRegexpAll()`, `extract()` built-in |
| Partitioning | none (manual sharding) | none (manual) | declarative range / list / hash | declarative range / list / hash | declarative `PARTITION BY` (OLAP-tuned, per-engine) |
| Concurrency model | file lock, single writer (WAL eases reads) | single writer | MVCC, multi writer | MVCC, multi writer (InnoDB) | append-optimized OLAP; immutable parts + async merges, no row-level locks |

Backend-only features that fall outside the portable subset: `WITHOUT ROWID`
(SQLite); `PIVOT`, `QUALIFY`, `ASOF JOIN`, `LIST` / `STRUCT` types, direct
Parquet / CSV reads (DuckDB); `JSONB`, `tsvector` full-text, declarative
partitioning (PostgreSQL); `FULLTEXT INDEX` and JSON path operators (MySQL);
the `MergeTree` engine family, incremental materialized views, `Array` /
`Tuple` / `Map` types, `ARRAY JOIN`, `SAMPLE`, `FINAL`, and approximate
aggregates (`uniq`, `quantile*`) (ClickHouse).

## User accounts and login

The user-domain schema lives in `res/sql/<backend>/1_user.sql`:

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

The chat-domain schema lives in `res/sql/<backend>/1_chat.sql`:

> **Timestamp convention.** Across the modern schema (`conversations`, `messages`,
> `files`, `users` + its log tables, `memories`) every `created_at` / `updated_at`
> / `deleted_at` is **unix milliseconds** in a `BIGINT` / `INTEGER` column, stamped
> by the app (`platform::now_unix_ms`) — no DB `DEFAULT CURRENT_TIMESTAMP` or
> `updated_at` triggers. `created_at` is `NOT NULL`; `updated_at` and `deleted_at`
> are NULL until the first update / soft-delete. Milliseconds because wearable
> health data is ms-native, so one scale avoids unit conversions. The legacy
> `th_*` tables keep their own DB-side `TIMESTAMPTZ`/`now()` columns.

- `conversations` — a **thin** per-conversation header: just `id`, `user_id`,
  `summary`, `created_at`, and a `deleted_at` soft-delete column, read back by
  `GET /api/history` / `POST /api/history/delete`. The modern `id` is **not
  auto-assigned** — it equals the conversation's opening question's `messages.id`,
  so a single id names the thread across both tables. The **legacy** backend keeps
  `th_sessions` with a *string* `session_id` key instead (and carries the question
  detail inline, no `messages` table), so the shared queries splice the table and
  key-column names per backend from `MIROBODY_CONVERSATIONS_TABLE` /
  `MIROBODY_CONVERSATION_ID_COL` ([`../chat/chat.hpp`](../chat/chat.hpp)).
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
  `(conversation_id, created_at)`, `(question_id)`, and `(user_id)`. On the modern
  backends `Chat::persist_history` writes the **opening question** row (and seeds
  the matching `conversations` row with its id) in one transaction; agent
  **responses** are not persisted yet, and the live multi-turn window is still
  cache-backed ([`../chat/history.hpp`](../chat/history.hpp); Redis / in-process).
- `files` — the per-user uploads index behind `GET /api/files`, documented in
  [`../chat/README.md`](../chat/README.md). **Not ported to MySQL:** its upsert
  relies on `ON CONFLICT (file_key) WHERE deleted_at IS NULL` plus a partial unique
  index, neither of which MySQL supports, so `/api/files` is unsupported on MySQL
  until a MySQL-specific upsert path lands.

`messages.role` is the `MessageRole` enum from [`enums.hpp`](enums.hpp), stored as
a `SMALLINT` — the same single-source-of-truth convention as `LoginMethod` /
`McpKind` above.
- **Reassign *every* table that references `users(id)`.** The list above is just
  the user-domain set; a real merge must move all other rows keyed by user too.
  When this is built, centralize it in a single `merge_users(keep_id, merged_id)`
  routine so a newly added table is updated in exactly one place and never
  silently left behind.
