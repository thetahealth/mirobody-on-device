# Storage

`mirobody::storage::Storage` is the object-store interface — `put_object` /
`get_object` / `delete_object` / `presigned_url` / `public_url` — that uploads,
charts and extracted text go through. It has one backend, `LocalStorage`, the
local filesystem: on a phone every object is a file in the app sandbox, which the
platform's backup and the record export both see. The cloud object stores (S3,
OSS, Azure Blob) belong to the server in the
[main mirobody repo](https://github.com/thetahealth/mirobody); they are preserved
at the `v2-full-2026-08` tag.

| Backend          | Class          | Signing            | Config getter         |
| ---------------- | -------------- | ------------------ | --------------------- |
| Local filesystem | `LocalStorage` | HMAC-SHA256 (opt.) | `cfg.local_storage()` |

The config struct exposes `configured()` and `open()`:

```cpp
auto cfg = mirobody::load_config();
if (cfg.local_storage().configured()) {
    std::unique_ptr<mirobody::storage::Storage> store = cfg.local_storage().open();
    std::string key = store->put_object("charts/abc.png", png_bytes, "image/png");
    std::string url = store->presigned_url(key);   // time-limited GET link
}
```

`put_object` (and `append_to_object` / `put_user_object` / `list_objects` /
`list_user_objects`) returns a **prefix-less** key. The configured key prefix
(`LOCAL_STORAGE_PREFIX`) is an internal
storage-access detail: `full_key()` applies it only when a method actually
reads or writes the backend, and it is never part of a returned key. So a
returned key is a portable handle that survives a prefix change (once the
objects are physically relocated) and that indexes / URLs / DB columns can hold
without baking in this deployment's prefix. URLs are minted from the key on
demand: `presigned_url` mints a time-limited signed GET **locally**, with no
network call; `public_url` is the unsigned CDN / bucket / mount URL. Keys passed
*in* may carry the prefix or omit it (`full_key()` prepends it only when absent,
idempotently), so older prefixed handles keep resolving. `LocalStorage`'s
`URL_PREFIX` is a separate, URL-only concern — see below.

## User objects

On top of the raw key/value operations, `Storage` manages **user uploads**
(the chat upload path in `chat::Dispatcher::store_attachments` is the main
caller):

```cpp
std::string ctype = mirobody::storage::resolve_content_type("photo.JPG", declared);
mirobody::storage::ObjectMeta meta;
meta.filename = "photo.JPG";
std::string key  = store->put_user_object(user_id, bytes, ctype, meta);
std::vector<std::string> keys = store->list_user_objects(user_id);
```

`put_user_object` stores the bytes under a derived key and returns it
(prefix-less; the configured key prefix is applied only at backend access):

```
<user seg>/<shard>/<digest>[.<suffix>]
```

Both hashed segments are **unpadded-base64url HMAC-SHA1 keyed by the
object-key seed** (27 chars of `[A-Za-z0-9_-]` each): the first over the decimal
user id — one prefix per user, hashed so URLs don't leak the sequential account
id — and the second over the file bytes, so repeat uploads of identical bytes
land on the same object (content-addressed dedup) while keys stay unguessable
without the bytes and the deployment config. `<shard>` is the digest's first
character: a 64-way fan-out so one user's objects spread across subdirectories
instead of piling into a single folder, which matters for the `LocalStorage`
filesystem backend (a directory with very many entries scans slowly). The `<suffix>` is a filename
extension derived from the Content-Type (e.g. `.pdf`; none when the type is
unknown), so `LocalStorage`'s extension-driven HTTP mount serves the object
as the type it was stored with.

`resolve_content_type(filename, declared)` picks that Content-Type
**extension-first** from a single extension↔type table shared with the suffix
derivation (so suffix and type always agree), then falls back to the
client-declared type verbatim, then `application/octet-stream`.

Alongside the object, `put_user_object` appends one record to the
**`<key>.meta` sidecar** — JSON Lines, one `{"content_type", "size",
"uploaded_at"}` line per upload of these bytes, plus whatever the optional
`ObjectMeta` carries (e.g. the original `filename`; empty fields are
omitted). Deliberately no user id — the hashed key segment exists so stored
objects don't carry the sequential account id. A re-upload appends a record
(single JSON documents are hard to append; `append_to_object` is native on
LocalStorage), so the first record is the original upload and the last
the current one. It is the durable per-file record: the chat
upload index (`src/transcode/file.*`) rebuilds a user's file list from these
sidecars on a cache miss, deriving everything else (presigned URL,
extracted-text key) server-side. It is named `.meta` rather than
`.json` because an upload key can itself end in `.json` (an
`application/json` upload stores as `<digest>.json`), and a name no upload
key can take keeps sidecars distinguishable by shape alone. `transcode/file`
re-derives the user segment and validates the exact key shape
(`key_shape_ok`), so the key scheme and that module must evolve together.

`list_user_objects(user_id, max_keys)` is the bounded `list_objects` scan of
the user's hashed prefix: the uploads plus their `.meta` / `.trans`
(extracted text) sidecars, in lexicographic order.

`LocalStorage` needs no credentials. `LOCAL_STORAGE_DIR` need **not** live under `HTTP_ROOT` — keeping uploads
outside the web root avoids serving them as ordinary static assets.

- **`LOCAL_STORAGE_URL_PREFIX`** — the URL path this server serves objects at,
  a URL concern only (**not** part of the stored path). With `/files`,
  `put_object("chart.png")` stores `LOCAL_STORAGE_DIR/chart.png` (flat under the
  dir) and serves it at `/files/chart.png`, the mount stripping `/files` to find
  the file (`Router::set_storage_mount`, wired in `server/server.cpp`). Empty ⇒ this
  server doesn't serve them.
- **`LOCAL_STORAGE_BASE_URL`** — the public prefix `public_url` /
  `presigned_url` return. Empty ⇒ a relative same-origin URL (`/files/chart.png`); set to an
  absolute origin (e.g. a CDN domain whose path aligns with `URL_PREFIX`) ⇒
  absolute URLs (`https://cdn/files/chart.png`), with the server still serving
  the bytes for the CDN to pull from.

With neither set, `public_url` returns a `file://` URL (no HTTP).

Set `LOCAL_STORAGE_SECRET` and `presigned_url` appends
`?expires=<unix>&sig=<hmac>`, where `sig` is
`HMAC-SHA256(secret, "<key>\n<expires>")`. The storage mount **verifies**
that signature (recomputing the same HMAC, in constant time, and checking the
expiry) before serving — a missing / expired / forged signature gets `403`.
Because the signature is over the object key (not the URL host/path), it
survives a CDN forwarding the query string to origin. The minted URL is cached
per `(key, lifetime)` for half the requested lifetime, so repeated calls hand
out one stable, downstream-cacheable URL. With no secret, objects under the
mount are served unconditionally; with no secret **or** no `URL_PREFIX` (this
server doesn't serve the object, so there's no mount to honour a signature)
there is nothing to sign against, so `presigned_url` equals `public_url` and the
expiry is ignored.

When signing **is** enforced (both `LOCAL_STORAGE_SECRET` and
`LOCAL_STORAGE_URL_PREFIX` set), no unsigned URL resolves — the mount `403`s it —
so `public_url` **throws** rather than return a dead link. Generic callers that
want a usable URL for any backend should prefer `presigned_url` (it collapses
to the public URL where signing doesn't apply).

The secret must be a real key: with a `URL_PREFIX` set, a `LOCAL_STORAGE_SECRET`
of fewer than 16 non-whitespace characters is **rejected** (the `LocalStorage`
ctor and the server's mount setup both call `LocalConfig::validate_signing_secret()`,
so the server refuses to start) — a blank or trivial key would make the
signatures forgeable while still reporting access as gated.

The `LOCAL_STORAGE_*` keys are listed in
[config.example.yml](../../config.example.yml).

The encoding and signing primitives live in [sign.*](sign.cpp) as pure functions
of their inputs (no clock, no network), verified against published test vectors
in [tests/storage/](../../tests/storage/sign_test.cpp).

## Encryption at rest

When `FILE_ENCRYPTION_KEY` is configured (one or more 44-char URL-safe-base64
[`encrypt::Fernet`](../config/fernet.hpp) keys), `Storage` stores user uploads
and their `.meta` / `.trans` sidecars **Fernet-encrypted**, so a copied
storage directory yields ciphertext rather than file contents — the keys
live in app config, separate from the storage secret. The server wires them once
after `open()` via `enable_file_encryption()`; with no key every method below is
a passthrough and behaviour is exactly as before.

The seam is three methods on `Storage`, used by the chat upload path and the
per-user index so writers and readers can't drift:

- `put_user_object` encrypts the object bytes and the `.meta` record. The object
  **key is still derived from the plaintext** (`hmac_sha1(seed, data)`), so
  content-addressed dedup survives — identical bytes still land on one key; only
  the stored body changes. The HMAC `seed` is the bucket name normally, but the
  `FILE_KEY_SEED`-derived secret while encryption is on (see `object_key_seed`),
  so the key can't be recomputed from the storage credential alone. An encrypted
  `.meta` is one Fernet token over the whole JSON-Lines body, so it can't be
  native-appended: the write reads, decrypts, appends the line, re-encrypts, and
  rewrites (the plaintext path keeps the native append).
- `put_object_encrypted` / `get_object_decrypted` are the write / read seams for
  a sidecar stored next to an upload (`.trans`) and for every user-content fetch
  (upload bytes, `.meta`, `.trans`). `get_object_decrypted` throws `StorageError`
  if the object is absent or no configured key decrypts it.

`signed_read_url(key)` is what callers hand a client (chat upload events, the
file index). With encryption **off** it is exactly `presigned_url` (bucket-direct
/ local mount). With encryption **on** the bytes must flow back through this
server decrypted — a client can't decrypt bucket bytes itself — so it mints a
URL to the app's own **decrypting file mount** at `FILE_URL_PREFIX` (optionally
absolute via `FILE_URL_BASE`), signed with an HMAC key derived from
`FILE_KEY_SEED`. The router mount (`Router::set_file_mount` /
[`serve_decrypted_file`](../server/router.cpp)) verifies that `?expires=&sig=`
exactly as the LocalStorage mount does, then fetches + decrypts the bytes and
serves them with a Content-Type inferred from the key (`text/plain` for a
`.trans` transcript). Because every backend now serves bytes through this mount,
the LocalStorage disk mount — which would send raw ciphertext via `sendfile` —
stands down while encryption is on.

### Two kinds of secret, and key rotation

`FILE_ENCRYPTION_KEY` (the cipher) and `FILE_KEY_SEED` (object-key paths) are
deliberately **separate**, which is what makes rotation cheap:

- **`FILE_ENCRYPTION_KEY` is a list** — the **last** key encrypts new writes, and
  **all** keys are tried on read (`encrypt_bytes` / `decrypt_bytes`). Rotate with
  zero downtime by **appending** a new key: objects written under an older key
  keep decrypting because their key is still in the list. Once nothing is still
  encrypted under an old key (let those objects expire, or re-write them under the
  newest key with the **`file_rekey`** CLI — see below), **drop** it. A dropped
  key whose objects still exist makes them unreadable, and that can't be
  auto-detected (it needs a full scan) — so removing a key is an operator
  responsibility.
- **`FILE_KEY_SEED` is a single, stable secret** that seeds the object-key paths,
  kept apart from the ciphers precisely so a cipher rotation leaves every path
  unchanged — the precondition for a multi-key read to *find* an object before
  trying keys on it. It is **required** when encryption is on and must **never**
  change once uploads exist (changing it re-paths everything, orphaning it).

Nothing is ever corrupted: the on-disk ciphertext is intact, so restoring a
removed key or the original seed recovers everything. *Losing* a key for good
(with objects still under it) is the only unrecoverable case — back these up like
a DB or JWT key.

To turn an accidental change into a safe, loud failure rather than silent
orphaning, the server records **non-secret fingerprints** (HMACs of the seed and
each key) in a `.file-encryption-check` marker on first run, and on every later
start `reconcile_key_marker` compares: a changed seed, or a key list that shares
**nothing** with the recorded set, makes the server **refuse to start** (see
`Server::start`); a normal rotation (at least one key still matches) is accepted
and grows the recorded set.

Under a leaked storage credential the file bytes, filename, and extracted text
all stay encrypted. Keying object-key derivation with the `FILE_KEY_SEED`-derived
secret (rather than the public bucket name) also denies the credential-holder the
ability to recompute a user's object keys, so the confirmation-of-file exposure —
"does this user have *this* file?", answerable when the seed is a value the
attacker knows — is closed while encryption is on. Mixing in further public
material (region, account id) would *not* close it, since the credential-holder
knows those too; only a secret seed does.

## CLIs

`file_rekey` ([cli/file_rekey.cpp](../../cli/file_rekey.cpp)) re-encrypts stored
uploads under the newest `FILE_ENCRYPTION_KEY`, in place, so an old key can be
dropped from the list after a rotation: it decrypts each object / `.meta` /
`.trans` with whatever configured key fits and rewrites it under the newest one
(paths are unchanged — they derive from `FILE_KEY_SEED`). `--dry-run` reports
the counts without writing; `--user ID` scopes to one user. Run it until it
reports zero failures before removing the retired key.
