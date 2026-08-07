# Configuration

## Local config

Config is resolved from three layers. They are merged per key, lowest precedence
first, so a higher layer overrides a lower one only for the keys it sets
(high → low):

1. **Environment variables** — applied at lookup time, so they override every
   file/remote value (see `ConfigStore::get_str`).
2. **`config.yml`** — the operator-created override file, located via
   `--config <path>` → `MIROBODY_CONFIG` env → `./config.yml` in the current
   directory. Not committed (it is `.gitignore`d); copy `config.example.yml` to
   create it. Absent is fine — when missing, only the template and remote apply.
3. **Remote config** — pulled from the config server when `CONFIG_SERVER` /
   `CONFIG_TOKEN` / `ENV` are set (see [Remote config](#remote-config)).
4. **`config.example.yml`** — the committed template, the source of defaults.
   Found *beside* the resolved `config.yml` path (e.g. `--config /etc/mb/x.yaml`
   loads `/etc/mb/x.example.yaml` underneath it). Loaded if present.

`ENV` no longer selects a local file — it drives only the remote pull. Anything
still unset falls back to the built-in `Config` defaults (see
[config.hpp](config.hpp)), which bind to `0.0.0.0:8080` with no API keys — so
you'll want a config file to talk to upstream providers.

Example `config.yml` (flat top-level keys, matching the Python sibling
project's format):

```yaml
HTTP_HOST: 0.0.0.0
HTTP_PORT: 8080
HTTP_URI_PREFIX: ''      # optional sub-path mount, e.g. '/mirobody'; see below
HTTP_MAX_BODY_BYTES: 33554432   # 32 MiB, the default; caps one upload. See below

LOG_LEVEL: info

GEMINI_API_KEY: ...
```

| Env var           | Purpose                                                              |
| ----------------- | -------------------------------------------------------------------- |
| `MIROBODY_CONFIG` | Path to a local YAML file (overridden by `--config` on the CLI).     |

### URI prefix (sub-path mounting)

`HTTP_URI_PREFIX` mounts the **whole app** — every HTTP route, every WebSocket
route, and the static web client — under a sub-path, so a single server can sit
behind a shared-host ingress at e.g. `https://host/mirobody/`. Empty/unset (the
default) serves everything at the root, exactly as before.

The value is normalized on load to a leading slash with no trailing slash
(`mirobody`, `/mirobody`, and `/mirobody/` all become `/mirobody`). With it set
to `/mirobody`:

- Every registered route is served under the prefix: `POST /mirobody/v1/chat`,
  `GET /mirobody/api/health`, `wss://host/mirobody/api/chat`, etc. The
  route tables under [HTTP API](../../README.md#http-api) / [WebSocket routes](../../README.md#websocket-routes)
  are all relative to the prefix.
- Static files are served with the prefix stripped (`/mirobody/assets/x.js` →
  `assets/x.js` under `HTTP_ROOT`). A request **outside** the prefix 404s — the
  app exists only under the mount, never at the bare root.
- `GET /mirobody` (no trailing slash) `302`-redirects to `/mirobody/`, so the
  browser resolves the SPA's relative asset URLs against the mount.
- The web client derives its API base from `window.location` at runtime (see
  `net.appBase()` in [htdoc/src/net.js](../../htdoc/src/net.js)), so one build works at
  any mount with no rebuild and no inline-script CSP exception.

**Deployment:** the ingress/reverse proxy must route `/mirobody/*` to the server
**without stripping** the prefix — the server expects to receive the full path.
For local `webpack serve` dev, keep `HTTP_URI_PREFIX` empty (the dev-server proxy
forwards unprefixed paths).

### Request body limit

`HTTP_MAX_BODY_BYTES` caps a single request body on **every** route; the default
is **33554432 (32 MiB)** and `0` removes the bound. It is the limit a chat file
upload actually hits, and the only thing bounding one: a body is buffered whole
in memory before it is dispatched, an upload's bytes are then copied down the
attachment chain, and inlining them into a model request base64-expands them by
a third — so one request costs several times this value at peak.

Enforced in two places (see `Router::set_max_body_bytes`):

- **At headers time**, against `Content-Length` — an oversized upload is refused
  before a single body byte is buffered.
- **As the body arrives**, against the running total — the only check that
  catches a chunked upload (which declares no length) or a `Content-Length` that
  understated the body.

Either way the response is `413` with the standard envelope, carrying the limit
so a client can report it:

```json
{ "code": -1, "msg": "request body too large", "data": { "max_bytes": 33554432 } }
```

The connection is then closed rather than kept alive — the client is still
sending a body nobody is reading, and the remainder would otherwise be parsed as
the next request.

**Raising it.** Keep it comfortably under the model providers' own inline caps
(Gemini rejects a request whose inline payload exceeds 20 MB), and remember a
reverse proxy has its own limit — nginx `client_max_body_size`, or the ingress
equivalent. The smaller of the two wins, so raise both or the proxy will reject
the upload first, with its own error page instead of the JSON envelope above.

## Object storage

Three object-store backends compile into every build; the active one is selected
at runtime by which keys are set. Read them via `cfg.s3()`, `cfg.oss("name")`,
and `cfg.local_storage()` — each returns a config struct with `configured()` and
`open()`. The interface and per-backend behavior are documented in
[src/storage/README.md](../storage/README.md); every key is in the "Object
Storage Configuration" block of [config.yml](../../config.yml).

A cloud backend (S3 / OSS) takes precedence; the local filesystem is the
**fallback**. When S3 or OSS is configured, the server's LocalStorage HTTP mount
stands down (those objects live in the cloud), so the `LOCAL_STORAGE_*` keys
apply only when no cloud backend is set.

The local-filesystem backend (`LOCAL_STORAGE_*`) serves uploads over HTTP from
`LOCAL_STORAGE_DIR` at the URL path `LOCAL_STORAGE_URL_PREFIX`, optionally signed
with `LOCAL_STORAGE_SECRET`. Two config-validation rules are worth calling out
because they fail **at startup**:

- `LOCAL_STORAGE_URL_PREFIX` is normalized to a leading-slash, no-trailing-slash
  form on load (`files`, `/files`, `/files/` all become `/files`); it is a URL
  path only, not part of the on-disk key.
- When both `LOCAL_STORAGE_URL_PREFIX` and `LOCAL_STORAGE_SECRET` are set
  (request signing is in play), the secret must be at least **16 non-whitespace
  characters** — the server refuses to start otherwise, since a blank or trivial
  key would make the presigned-URL signatures forgeable. Leave the secret unset
  to deliberately serve objects unsigned (public).

## Remote config

When the environment provides all three of `CONFIG_SERVER`, `CONFIG_TOKEN`,
and `ENV`, mirobody will pull resolved YAML from
`{CONFIG_SERVER}/api/v1/config/environments/{ENV}/configs/resolved?is_yaml=true`
(sending `X-Config-Token: {CONFIG_TOKEN}`). Remote values are layered **over**
the `config.example.yml` template (so remote overrides the defaults), and the
operator's `config.yml` is in turn layered over remote — see the precedence
list under [Local config](#local-config). Environment variables still win over
everything.

| Env var         | Purpose                                                                |
| --------------- | ---------------------------------------------------------------------- |
| `CONFIG_SERVER` | Base URL of the config service.                                        |
| `CONFIG_TOKEN`  | Bearer token sent in the `X-Config-Token` header.                      |
| `ENV`           | Environment name (e.g., `dev`, `staging`, `prod`) used in the URL.     |

## Encrypted values

Any string value beginning with `gAAAA` is treated as a Fernet token and
decrypted on load, provided `CONFIG_ENCRYPTION_KEY` is set in the
environment. Applies to **both** the local YAML and the remote pull, so
secrets can be stored encrypted at rest in either source. The encryption
key is passed through the same derivation as the Python loader (trim,
pad/truncate to 32 bytes with ASCII `'0'`, then URL-safe base64), so the
same key string works on both sides.

| Env var                 | Purpose                                                          |
| ----------------------- | ---------------------------------------------------------------- |
| `CONFIG_ENCRYPTION_KEY` | Raw secret derived into the Fernet key that decrypts `gAAAA…` values. |
