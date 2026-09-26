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
- A web client should derive its API base from `window.location` at runtime, so
  one build works at any mount with no rebuild and no inline-script CSP exception.

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

## Google: AI Studio or Vertex AI

The Gemini models are reachable on two surfaces, and **naming a project and a
location is what selects Vertex** — for every Google call the server makes, chat
and embeddings alike. Leave either unset and both stay on AI Studio.

| Key | Meaning |
| --- | --- |
| `GOOGLE_API_KEY` | AI Studio key. All that is needed for the default surface. |
| `GOOGLE_CLOUD_PROJECT` | Vertex project id. Together with the next key, the switch. |
| `GOOGLE_CLOUD_LOCATION` | Where the model runs — see [Locations](#locations). |
| `GOOGLE_CLOUD_MODEL_LOCATIONS` | Per-model overrides of the above — see [One location cannot serve every model](#one-location-cannot-serve-every-model). |
| `GEMINI_BASE_URL`, `GEMINI_AI_STUDIO_BASE_URL`, `GEMINI_VERTEX_BASE_URL` | Host overrides, for a proxy or gateway in front. |

Those keys pick the surface; they say nothing about *who* the server is when it
gets there. Vertex needs a credential besides, and the next section is the whole
of that story — on GCP it is nothing at all.

### How Vertex authenticates

Every Vertex call carries an OAuth bearer token, and there are five ways this
server can come by one. They are tried **in this order**, first hit wins —
`gcp::TokenSource` picks among the top three, and Application Default Credentials
resolves the rest exactly as `google.auth.default()` does
([../client/gcp_auth.hpp](../client/gcp_auth.hpp)). Nothing here needs configuring
on GCP: leave all of it unset and row 5 answers.

| # | Mechanism | Configured by | Use it when |
| --- | --- | --- | --- |
| 1 | **Token file** | `GCP_ACCESS_TOKEN_FILE` | Something outside the server keeps a raw token current — a sidecar, a projected volume. Re-read on every request, so a rotation needs no restart. |
| 2 | **Inline token** | `GCP_ACCESS_TOKEN` | Poking at a local run with `gcloud auth print-access-token`. Read once at startup, so it expires with the token (~1h) and every turn fails on auth after that. Not a deployment. |
| 3 | **Credential file** | `GOOGLE_APPLICATION_CREDENTIALS` | Off GCP. One path, three kinds of file — see below. |
| 4 | **gcloud ADC login** | nothing — the well-known path | A developer machine that has run `gcloud auth application-default login`. Same reader as row 3; the file is just found rather than named. |
| 5 | **Metadata server** | nothing | On GCE / GKE / Cloud Run. The instance's attached service account, no key to store and none to rotate. **The production answer on GCP.** |

Rows 1 and 2 short-circuit the rest: set `GCP_ACCESS_TOKEN_FILE` and a
`GOOGLE_APPLICATION_CREDENTIALS` sitting next to it is never opened. That is the
intent — an explicit token is an override — but it also means a stale token file
left over from debugging silently outranks a correct credential. The startup log
says which mechanism won (`TokenSource::describe`, then `gcp auth: …` for the ADC
sources); read it before debugging a `401`.

Rows 3–5 are Application Default Credentials, and rows 3 and 4 are **one slot,
not two steps**: `GOOGLE_APPLICATION_CREDENTIALS` if set, otherwise the path
`gcloud` writes (`%APPDATA%\gcloud\application_default_credentials.json` on
Windows, `~/.config/gcloud/…` elsewhere). Whichever file is found, its `type`
field decides the grant:

| `type` | Grant | Where it comes from |
| --- | --- | --- |
| `service_account` | Signed JWT (RFC 7523) against the key's `token_uri` | A downloaded key. Works anywhere; it is also a long-lived secret you now own. |
| `authorized_user` | `refresh_token` grant | `gcloud auth application-default login`. Your own account, not a workload's — fine for a laptop, wrong for a server. |
| `external_account` | STS token exchange | Workload identity federation. **Prefer this off GCP** — no key exists to leak or rotate. |

A file whose `type` is none of the three, or which is missing the fields its
grant needs, is logged and skipped — the server falls through to the metadata
server, which off GCP cannot answer. That combination (`GOOGLE_APPLICATION_CREDENTIALS`
pointing somewhere unreadable, then a metadata timeout) is the usual shape of a
Vertex deployment that authenticates on a laptop and not in its container.

Two failure modes worth naming because each looks like the other's opposite:
credentials without `GOOGLE_CLOUD_PROJECT` + `GOOGLE_CLOUD_LOCATION` leave the
server quietly on AI Studio, and those two without a credential is the
`Vertex mode has no access token` error.

### Workload identity federation, off GCP

Point `GOOGLE_APPLICATION_CREDENTIALS` at the `external_account` file `gcloud iam
workload-identity-pools create-cred-config` writes: no service-account key is
stored and no key has to be rotated. Which `credential_source` that file carries
decides how the workload proves who it is, and the choice is not a detail — one
of the two shapes cannot work in a container at all.

*A token file* (`credential_source: {file: ...}`) is the shape to want. The
workload's own platform writes a signed OIDC token somewhere and this server reads
it, re-reading per token mint so a rotation needs no restart. On Kubernetes that
is a projected `serviceAccountToken` volume whose `audience` matches the Google
provider, federated against the cluster's OIDC issuer — nothing about AWS is
involved, and it works identically on EKS, GKE and anything else running
Kubernetes. Needs an **OIDC** provider on the Google side; an AWS provider only
accepts SigV4 signatures and will reject a JWT.

*`aws1`* signs an AWS `GetCallerIdentity` request with SigV4 and lets Google's STS
replay it, exactly as `google.auth.aws` does (checked byte for byte in
[../../tests/client/gcp_auth_test.cpp](../../tests/client/gcp_auth_test.cpp)). The
AWS credentials that sign it are looked for in three places, in order:

1. **The environment** — `AWS_ACCESS_KEY_ID` + `AWS_SECRET_ACCESS_KEY` (+
   `AWS_SESSION_TOKEN`), taken only when both halves are present. With
   `AWS_REGION` set too this needs no metadata server at all, which is what makes
   `aws1` work on Lambda.
2. **IRSA** — `AWS_ROLE_ARN` + `AWS_WEB_IDENTITY_TOKEN_FILE`, the identity a
   Kubernetes pod on EKS actually has. The projected token is exchanged for
   temporary credentials through `AssumeRoleWithWebIdentity`, which needs no
   signature of its own. **This step goes beyond `google.auth.aws`**, whose
   built-in credential source reads only the metadata server; google-auth leaves
   the case to `AwsSecurityCredentialsSupplier`, a hook each application
   implements (the Python service in this stack does it via boto3). Doing it here
   means a deployment configures it once and the credential file needs no
   `credential_source` changes.
3. **EC2's instance metadata server** — the instance profile. Reachable from an
   instance, usually not from a pod: EKS both requires IMDSv2 and leaves the
   metadata hop limit at 1, so a container's `PUT /latest/api/token` is dropped
   while unauthenticated reads answer `401`. That deadlock is why step 2 exists.
   On an instance that does require IMDSv2, the credential file must name
   `imdsv2_session_token_url` (`gcloud ... create-cred-config --enable-imdsv2`);
   including it is always safe, since IMDSv2 works even where v1 is still allowed.

**ECS and Fargate** serve the task role at `169.254.170.2`, an address neither
this implementation nor `google.auth` reads — export the environment variables in
step 1 there.

Both shapes support `service_account_impersonation_url`, which swaps the
federated token for a service account's once the exchange succeeds — and both
still need `GOOGLE_CLOUD_PROJECT` and `GOOGLE_CLOUD_LOCATION` besides. Splitting
those two halves across two deployments is a quiet way to have neither work.

### Locations

**`GOOGLE_CLOUD_LOCATION` is not any region you like.** Each model publishes its
own list, and a value outside it has no node at all — `gemini-3.6-flash` serves
`global` only, while `gemini-3.5-flash` adds the `us` / `eu` multi-regions and a
few single regions. Use `us` (**not** `us-east5`) to keep inference in the United
States, `eu` for the EU, or `global` for Google's routing, which is about 10%
cheaper with no residency guarantee. The model picker is built per surface, so a
Vertex deployment is offered only what Vertex can answer.

The location also picks the endpoint host, and the three kinds of location spell
it three different ways:

| Location | Host |
| --- | --- |
| a region, e.g. `us-central1` | `us-central1-aiplatform.googleapis.com` |
| a multi-region, `us` or `eu` | `aiplatform.us.rep.googleapis.com` |
| `global` | `aiplatform.googleapis.com` |

The multi-regions are Representative Endpoints, so the location is an **infix**,
not a prefix — `us-aiplatform.googleapis.com` is not an endpoint, and Google
answers it with `400 INVALID_ARGUMENT: Invalid hostname`. Nothing falls back.
`gcp::vertex_host` ([../client/gcp_auth.hpp](../client/gcp_auth.hpp)) is the
single place that knows the rule; the path after the host is identical for all
three.

### One location cannot serve every model

`GOOGLE_CLOUD_LOCATION` is a **default**, not the whole answer, because a location
is a property of the *model*. The published lists do not merely differ between
models — they invert:

| | `us` / `eu` multi-region | US single regions |
| --- | --- | --- |
| `gemini-embedding-2` | ✅ | ❌ |
| `gemini-embedding-001` | ❌ | ✅ (all seven) |
| `gemini-3.5-flash` | ✅ | ❌ |
| `gemini-2.5-flash` | ❌ | ✅ |

Read down either column: **no single value serves everything this server offers.**
`us` gives you chat on 3.5-flash and a 404 on both `gemini-embedding-001` and
`gemini-2.5-flash`; a single region trades one set of 404s for the other. That is
not a misconfiguration waiting to be corrected — it is an unsatisfiable constraint.

`GOOGLE_CLOUD_MODEL_LOCATIONS` breaks it: a map from model id to the location that
model is reached at, consulted by both lanes, with `GOOGLE_CLOUD_LOCATION` as the
fallback for anything it does not name.

```yaml
GOOGLE_CLOUD_LOCATION: 'us'          # the fallback, for models not named below
GOOGLE_CLOUD_MODEL_LOCATIONS:
  gemini-embedding-001: 'us-west1'
  gemini-2.5-flash: 'us-west1'
  gemini-3.5-flash: 'us'
```

**This map ships live in [config.example.yml](../../config.example.yml)** while the
rest of the Vertex block stays commented, because the rest are switches and this is
correctness: a fresh clone that enables Vertex and takes the recommended `us`
would 404 on two models, which reads as "Vertex is broken" rather than "one line
is missing". Nothing reads the map until Vertex is on, so an AI Studio deployment
carries it for free. All three models are pinned explicitly rather than letting
the multi-region one ride the fallback, so changing `GOOGLE_CLOUD_LOCATION` later
moves only what was never pinned.

As an environment variable it is the same map as JSON — and an env value
**replaces** the file's map rather than merging into it, so name every model there
too: `GOOGLE_CLOUD_MODEL_LOCATIONS='{"gemini-embedding-001":"us-west1"}'`.

**Nothing is validated and nothing is inferred**, both on purpose.
`gcp::vertex_model_location` holds no table of what each model serves: Google's
lists move, and a stale table compiled in here would be a 404 no operator could
override, where a wrong map entry is Google's own 404 naming the model and the
location. Nor does it resolve `us` to a nearby region for a model that lacks it —
that would move data across a residency boundary the operator chose deliberately.
The startup log prints every override in force, which is the first thing to read
after a `Publisher model ... was not found` error.

One thing this does reach into: `gemini-3.5-flash` is about 10% cheaper on
`global`, so its price is computed from the location it will actually be reached
at rather than the deployment's
([../../res/agents/baseline.cpp](../../res/agents/baseline.cpp)).

`GCP_PROJECT` / `GCP_LOCATION` named those two keys in earlier revisions and are
no longer read — rename them. The server warns at startup if either is still set,
since a project that resolves to nothing is indistinguishable from a deployment
that never wanted Vertex. Every key is documented in
[config.example.yml](../../config.example.yml).

## Object storage

One backend: the local filesystem. Read it via `cfg.local_storage()`, which
returns a config struct with `configured()` and `open()`. The interface is
documented in [src/storage/README.md](../storage/README.md); the keys are in
[config.example.yml](../../config.example.yml).

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
