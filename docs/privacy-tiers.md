# Privacy Tiers — Where the Model Runs, Where the Data Lives

Health data is the most sensitive thing this app touches, so the question every user
actually asks is not "which model do you use?" — it is **"does my data leave my
phone?"** Answering that needs both halves of the system at once, which is why
inference and storage are documented together here rather than in two files a reader
would have to cross-reference to feel safe.

There are exactly **two independent choices**, and both default to staying on the
device:

- **Where the model runs** — the *inference lane*: on-device, a home server, or a cloud
  provider. Chosen per turn by a router. → [Part 1](#part-1--the-inference-lanes)
- **Where the data lives** — the *storage tier*, which is just the **Backend** setting
  already in the app's nav drawer: this device, a home server, or a hosted deployment.
  Chosen once in settings. → [Part 2](#part-2--the-storage-tiers)

They are orthogonal on purpose. Sending one hard question to a cloud model does not
move your health history there, and keeping a cloud backup does not mean a provider
sees your chats.

## The one-page answer

Rows are where the data lives (Backend); columns are where the model runs. Every cell is
a shipping configuration, not a hypothetical. ✅ nothing leaves your home · ⚠️ something
leaves, named per cell · ⛔ both model and data are in the cloud:

| Data ↓ / Model → | On-device | Home server | Cloud |
|---|---|---|---|
| **This device** | ✅ **nothing leaves the device** — fully offline, the default | ✅ the turn goes to the home box; nothing leaves the household | ⚠️ the turn **and any health context in it** go to a provider; your history stays on the device |
| **Home server** | ✅ history stays home, turns stay on the phone | ✅ **nothing leaves the household** — model and data on one box | ⚠️ the turn **and any health context in it** go to a provider; your history stays home |
| **Hosted (cloud)** | ⚠️ your history is in the cloud (encrypted at rest); turns stay on the phone | ⚠️ turns stay in the household; your history is in the cloud | ⛔ both in the cloud — most capable, least private |

**The whole green quadrant is the privacy pitch:** keep *both* choices at device-or-home
and nothing this app touches ever leaves your home network — the top-left cell narrows
that to the device itself. Every ⚠️/⛔ cell is a deliberate trade the user makes for
capability or durability, and the app says so at the moment of the trade (see
[Escalation UX](#escalation-ux)).

### What actually leaves the device, per artifact

Vague promises don't reassure anyone. This is the concrete list:

| Artifact | On-device tier | Home-server tier | Cloud tier |
|---|---|---|---|
| The turn you type | stays | to the home box (LAN) | **to the provider**, in the clear (TLS in transit; the provider sees the plaintext) |
| Health context injected into a turn (RAG) | stays | to the home box | **to the provider** — this is the real exposure, not the question text |
| Chat history | device SQLite | home Postgres | hosted DB |
| Uploads (photos, PDFs, lab reports) | device filesystem | home disk / home MinIO | S3 / OSS, **Fernet-encrypted at rest** |
| Wearable + clinical records (FHIR) | device SQLite | home Postgres | hosted DB; vendor OAuth tokens stored encrypted |
| Your provider API key | never leaves the secure store | never leaves the secure store | never leaves the secure store |

One party the matrix hides: on a **shared-key** cloud lane the turn passes through the
trusted server *before* the provider, so two parties see the plaintext instead of one.
BYOK's private-key lane is the only cloud option where the provider is the sole reader.

Three honest caveats, stated here rather than buried:

- **A cloud model sees what you send it.** Encryption at rest protects stored data from a
  leaked storage credential; it cannot hide a turn from the provider that must read it to
  answer. That is exactly why the router defaults to local and asks before escalating.
- **On-device-only means device loss is data loss.** There is no backup in the fully
  offline tier — the reason the storage tiers exist at all is durability and multi-device,
  not capability.
- **"Stays in the household" is a household-level promise.** Whoever administers the home
  box can read its disk. Inside a family that is usually the point; it is not the same
  guarantee as on-device, and the UI must not imply it is.

## Two axes, two settings

The pile of scenarios (local vs remote model, private vs shared key, C API vs HTTP,
device DB vs server DB) collapses into two settings the user can reason about:

| | Setting | Chosen | Granularity | Failure mode |
|---|---|---|---|---|
| **Model** | inference lane / provider | per turn, by the router | stateless — any turn may route differently | degrade to a nearer lane, retry |
| **Data** | **Backend** | once, in settings | stateful — one source of truth | cannot "fall back"; the data is where it is |

That difference is the whole reason storage isn't just another router: **inference is
stateless, storage is not.** A misrouted turn costs a retry; a mis-placed dataset costs a
migration. So the model side gets a router, and the data side gets a *placement policy*
plus (eventually) a sync protocol.

Where they meet: **a home server can be both** — the 32B model and the household's data
on one box, so retrieval-augmented answers never leave the LAN. That diagonal is the
sweet spot of the whole design.

**A third axis, deliberately out of scope here: sharing with other people.** Care circles
let a user share a conversation or their health data with someone they trust — opt-in, off
by default, and the assistant is read-only over another member's data. That is an
access-control question rather than a placement question, so it lives in
[README → Care circles](../README.md#care-circles). Nothing in this document grants another
person access to anything.

## Guarantees we hold ourselves to

These are hard rules, not aspirations, and they apply in every tier:

1. **A user's provider key lives on-device only**, in the platform secure store (Keychain
   / Keystore / Windows Credential Manager / HarmonyOS ASSET Kit), addressed by provider
   id. **Never** send it to any Mirobody server, log it, or embed it in telemetry or
   crash reports. A *shared* key stays in a server's config and never reaches a client.
2. **Escalation is never silent.** Routing a turn off-device is visible and, on the first
   occurrence, consented. Health-sensitive content is never escalated to a cloud lane
   unless the user has opted into cloud at least once.
3. **Stored user content is encrypted at rest** wherever a `FILE_ENCRYPTION_KEY` is
   configured — upload bytes, the filename sidecar, and extracted text — so a leaked
   S3/OSS credential yields ciphertext. Object keys are derived with a secret seed, so a
   credential holder cannot even confirm *whether* a given file exists for a user.
4. **The account id never appears in a storage path.** Per-user prefixes are keyed HMACs,
   so URLs don't leak a sequential account id.
5. **Degrade, don't crash.** A quota, key, or reachability failure surfaces a clear
   message and falls back to a nearer lane — it never drops a turn silently.

---

# Part 1 — The inference lanes

One private chat. A given turn can be answered by a model in one of four places, and a
single **router** decides per turn:

- **Local (default):** the on-device 4B model — engine, models, and hardware findings in
  [on-device-llm.md](on-device-llm.md). Low latency, offline, zero cost, data never
  leaves the device.
- **Home server (self-hosted):** the same C++ core / llama.cpp engine running as an
  OpenAI-compatible server on a machine the household owns, loading a 32B-class model.
  Near-cloud answer quality without the 2B/4B hallucination rate; data never leaves the
  home. See [The home-server tier](#the-home-server-tier).
- **Cloud, private key (BYOK):** a cloud provider on the *user's own* API key.
- **Cloud, shared key:** the same providers reached *through a trusted server* (e.g. a
  self-hosted Mirobody server) that holds the key — see [config.example.yml](../config.example.yml),
  which registers these provider keys server-side. For users who won't create their own
  key but trust a deployment that has one.

Escalation order is distance from the user: local first, the home server if one is
configured, cloud only past that.

## The lane model

On mobile the app can use a local model, a self-hosted server's model, or a remote model
on a private or shared key; and it binds to a model either through an in-process C API
or over HTTP. That sounds like a pile of application scenarios — it is actually just
**two orthogonal axes**, and every scenario is one cell:

**Axis 1 — where the model runs, and whose key:**

| Lane | Model runs on | Key | Who pays quota |
|---|---|---|---|
| **local** | the device | none | nobody |
| **home-server** | a machine the household owns (LAN / VPN) | none, or a locally issued token | the home hardware — no provider limits |
| **cloud-private** (BYOK) | provider cloud | the user's own key, in the on-device secure store | the user — a free tier or their own paid account |
| **cloud-shared** | provider cloud, via a trusted server | a shared key held by that server, never on the client | the deployment's quota |

**Axis 2 — how the app binds to the model:**

| Binding | What it is | Used by |
|---|---|---|
| **in-process C API** | llama.cpp / LiteRT-LM linked into the app | local lane on mobile |
| **HTTP (OpenAI-compatible)** | `POST …/chat/completions` + SSE | home-server and both cloud lanes; also a local Ollama-style server on desktop |

Not every combination exists: the home-server and cloud lanes are always HTTP; the local lane is C API
on mobile and may be either in-process or a local HTTP server on desktop. The binding is
a property of the *platform + lane record*, not a new scenario.

**One contract above the adapter.** Every lane, whatever its binding, is normalized
behind the same interface: OpenAI Chat messages in, a stream of content deltas out. The
HTTP lanes speak it natively; the local C-API lane has a thin adapter that maps engine
callbacks onto the same delta stream. Router, chat engine, history, and UI are
lane-agnostic — they never branch on where the model is.

What used to read as distinct application scenarios — offline chat, private everyday
Q&A, deep-thinking escalation on your own key, a family server running a 32B model, a
household/team sharing one deployment's quota, CN vs global provider defaults — are
**not separate code paths**. Each one is a router decision plus a lane record:

```jsonc
// A lane record describes ANY cell. Supersedes the old separate provider record.
{
  "id": "nvidia",                 // stable internal id
  "label": "NVIDIA NIM",          // shown in UI
  "kind": "remote",               // local | remote
  "binding": "http",              // http | c-api
  "key_scope": "user",            // none | user (BYOK, secure store) | shared (server-held)
  "base_url": "https://integrate.api.nvidia.com/v1",  // http only
  "model": "deepseek-ai/deepseek-v4-flash",
  "auth_scheme": "bearer",        // bearer | header:<name> | query:<name>
  "region_hint": "global",        // global | cn — for default selection
  "compatible": "openai-chat"     // openai-chat | adapter:<name>
}

// The local lane is the same record shape:
{ "id": "local", "label": "On-device", "kind": "local", "binding": "c-api",
  "key_scope": "none", "model": "qwen3.5-2b-q4_k_m", "compatible": "adapter:local" }

// A home server is just another HTTP lane — the model happens to live down the hall:
{ "id": "home-server", "label": "Family server", "kind": "remote", "binding": "http",
  "key_scope": "shared", "base_url": "http://192.168.1.20:8080/v1",
  "model": "qwen3-30b-a3b", "auth_scheme": "bearer", "compatible": "openai-chat" }

// A shared-key lane differs from BYOK only in key_scope and base_url:
{ "id": "mirobody-server", "label": "Mirobody server", "kind": "remote",
  "binding": "http", "key_scope": "shared",
  "base_url": "https://my-server.example/v1", "model": "auto",
  "auth_scheme": "bearer", "compatible": "openai-chat" }
```

Active selection persisted per user:

```jsonc
{ "provider_id": "nvidia", "model": "deepseek-ai/deepseek-v4-flash" }
// key is NOT here — a user key lives in the secure store, addressed by provider_id;
// a shared key never reaches the client at all.
```

## Why tiered, not either/or

Capability first — in *this* table ✅/⛔ mean "can it do the job", not "is it private"
(hence cloud scoring ✅ here and ⛔ in the privacy matrix above); the privacy row spells
its own verdict out in words:

| | Local | Home server | Cloud · private key | Cloud · shared key |
|---|---|---|---|---|
| Everyday Q&A, rewrite, summarize | ✅ ideal | fine, but local is closer | overkill | overkill |
| Long context, multi-step reasoning, code | ⛔ struggles | ✅ 32B-class | ✅ | ✅ |
| Fresh knowledge | ⛔ | ⛔ (no live web) | ✅ | ✅ |
| Latency | instant | LAN-fast | network-bound | network-bound |
| Cost to us / user | zero | owned hardware + electricity | zero to us; user's free tier or paid balance | the deployment's quota |
| Privacy | data stays on device | stays within the household | **leaves the device** to the provider | **leaves the device** to server + provider |
| Offline | ✅ | ✅ within the home LAN | ⛔ | ⛔ |
| Onboarding friction | model download | one household member sets up the box once | user registers + creates a key | none (server admin did it once) |

Default to local for privacy, latency, and offline. Escalate only when the task needs
it — and escalation crosses a privacy boundary, so it is never silent (see
[Escalation UX](#escalation-ux)).

## The pipeline

A turn flows through one pipeline; the **router** is a single replaceable stage that
picks the lane. Everything downstream is identical regardless of choice.

```
turn ─▶ router.select(turn, ctx) ─▶ { lane record } ─▶ adapter (OpenAI Chat deltas) ─▶ stream
                                          │
                    local (c-api) ────────┤
                    home-server (http) ───┤
                    cloud-private (http) ─┤
                    cloud-shared (http) ──┘
```

Keeping the router as one stage (not branches scattered through the call site) means the
strategy below can be swapped without touching transport or UI.

## Routing strategy

Who decides "local or cloud" for a given turn. Strategies, increasing complexity:

| Strategy | How it decides | Pro | Con |
|---|---|---|---|
| **Manual** | A "deep thinking" toggle in the UI | Simplest, explainable, no misroute | User must know when to flip it |
| **Heuristic** | Input length / keywords (code fences, "analyze/reason") / attached docs | No extra model, fast | Brittle rules, misroutes |
| **Local self-assess** | 4B answers first + emits a confidence / "unsure" signal; escalate below threshold | Reuses local model, natural | 4B self-assessment is unreliable |
| **Classifier route** | A tiny dedicated model/ruleset judges difficulty | Accurate | One more component to maintain |

### Decision: ship Manual + Heuristic fallback first

- Default lane is **local**.
- Escalate to cloud when a clear "hard" signal fires: over-long input, contains a code
  block, user tapped the deep-thinking toggle, or (later) local confidence is low.
- Which escalation lane: the nearest configured one — the home server if the household
  runs one, else BYOK / shared-key cloud per the user's settings. This is a settings
  choice, not a per-turn decision.
- Defer self-assessment and the classifier to v2 — add them once real usage shows where
  misroutes actually happen. The router is a replaceable stage, so this is a swap, not a
  rewrite.

## Router interface

```jsonc
// input
{
  "messages": [...],        // the conversation so far
  "ctx": {
    "has_attachments": false,
    "input_tokens_est": 320,
    "user_forced_lane": null,   // "local" | "cloud" | null
    "offline": false,           // no internet — kills cloud lanes; the LAN may still be up
    "home_server_configured": false,
    "cloud_configured": true,   // any cloud lane usable? (key present or server set)
    "context_is_health": false  // does this turn carry health data? (see the coupling section)
  }
}
// output
{
  "lane": "local",            // "local" | "cloud"
  "provider_id": null,        // set when lane == "cloud" — a lane-record id
  "reason": "default"         // for telemetry + the escalation notice
}
```

Guard rails independent of strategy:

- Reachability is **per lane**, not global: a home server on the LAN stays reachable
  with the internet down, so `offline` disqualifies only the cloud lanes. No escalation
  lane configured *and reachable* → force **local**.
- `user_forced_lane` always wins.
- Escalation lane unreachable / `429` / key invalid mid-turn → fall back to the next
  lane down (home server → local) with a notice.
- `context_is_health` without a prior cloud opt-in → force **local** or **home-server**
  (guarantee 2 above).

## Escalation UX

Escalation moves the conversation off-device, so it must be visible and, ideally,
consented:

- When routing to cloud, tell the user plainly: *"Handing this to the cloud model — it
  will use your <provider> key and this message leaves your device."* For a shared-key
  lane, name the server instead of the key: *"…via <server>, on its shared quota."*
- The home-server boundary is softer but still a boundary — the turn leaves the device:
  *"Handing this to your family server — it stays within your home network."*
- When health context is attached, say what is going, not just where: *"…including
  <N> records from your health history."* Vague notices are what erode trust.
- Offer a "keep it local" opt-out on that turn.
- Make the default lane and the deep-thinking toggle discoverable, not buried.
- Never escalate health-sensitive content without the user having opted into cloud at
  least once.

## The local lane

Engine, model lineup, packaging, and the per-platform hardware findings (CPU `i8mm` vs
GPU/NPU, bandwidth ceiling) are in [on-device-llm.md](on-device-llm.md). What matters
here is only the contract: the C-API adapter emits the same delta stream as the HTTP
transport, requires no key, and reasoning models that emit `<think>…</think>` inline are
handled by the same UI fold logic as the cloud lanes' reasoning output (see
[Provider quirks](#provider-quirks-adapter-notes)).

## The home-server tier

A household deploys one machine that runs a 32B-class model and serves every family
member's app over the LAN (or a VPN when away). It closes most of the quality gap to
cloud models — far fewer hallucinations than the 2B/4B on-device tier — while keeping
the data inside the home. Two properties make it nearly free to add:

- **The engine already exists.** The shared C++ core embeds llama.cpp for the local
  lane; llama.cpp also ships `llama-server`, an OpenAI-compatible
  `/v1/chat/completions` + SSE server with parallel slots and continuous batching —
  enough to serve several concurrent household clients. Standing this tier up is
  packaging (a server build of the core, or `llama-server` directly), not a new engine.
- **The client needs nothing new.** From the app's perspective this is just an HTTP lane
  record (see [the lane model](#the-lane-model)) — the same transport, discovery, and
  error handling as any cloud provider.

The same box is usually also the storage tier's home server ([Part 2](#part-2--the-storage-tiers)),
which is what makes household-local RAG possible.

### Hardware reality — what a home box can actually serve

The bandwidth law from [on-device-llm.md](on-device-llm.md) applies unchanged: decode
speed ≈ memory bandwidth ÷ bytes read per token. That yields two sweet spots:

| Option | Resident size (Q4) | Typical home hardware | Expected decode |
|---|---|---|---|
| **Dense 32B-class** (Qwen 32B, Gemma 27B) | ~19–20 GB | a 24 GB GPU (RTX 3090/4090) or a 32 GB+ unified-memory Mac | ~25–40 tok/s on GPU; ~3–5 tok/s CPU-only — not usable |
| **MoE ~30B total, ~3B active** (Qwen3-30B-A3B class) | ~18–19 GB resident, but only ~2 GB read per token | a 32 GB-RAM mini PC, **CPU only** | ~10–20 tok/s — usable |
| 100B+ MoE | 64 GB+ | beyond most homes | not the default |

So: with a 24 GB GPU, run a dense 32B; without one, a low-active-param MoE on a plain
32 GB mini PC is the budget path. Caveat for CPU-only boxes: *prefill* is compute-bound,
not bandwidth-bound, so long-document inputs are slow — even an entry-level GPU used
only for prefill offload pays for itself.

### What it buys, and what it doesn't

- Quality ≈ a cloud flagship of a year or two ago; hallucination much reduced vs 2B/4B
  but **not zero** — health-sensitive answers still want grounding/RAG eventually.
- No fresh knowledge without tools; the cloud lanes keep that advantage.
- Quota is the household's own hardware: no rate limits, no free-tier ToS exposure.
- Works with the internet down — LAN reachability is all it needs.
- **Privacy is household-level, not per-person.** Whoever administers the box can read
  its disk. Inside a family that is usually fine; it is not the same guarantee as
  on-device, and the UI should not imply it is.

Serving checklist (open questions below): LAN discovery, away-from-home access
(VPN/Tailscale + TLS), a minimal bearer token even inside the home, model management on
the box, and a KV-cache budget per concurrent user.

## The cloud lanes — provider contract

**BYOK = Bring Your Own Key.** In the private-key lane, the app ships as a client only:
each end user pastes their *own* API key, and inference runs on that provider under the
user's own quota — a free tier for most users, or a **paid account** for those who want
frontier models and higher limits. BYOK is about *whose key*, not about price. Either
way we host no inference, consume no quota, and bill nobody.

The shared-key lane is the same wire contract pointed at a trusted server: the server
(config: [config.example.yml](../config.example.yml)) holds provider keys and exposes an
OpenAI-compatible endpoint; the client authenticates to the server and never sees the
provider key. Everything below about request shape, discovery, quirks, and errors
applies to both; key-handling rules apply to whoever holds the key.

### Why BYOK works here

- The per-user free quotas below (e.g. 250 requests/day) are **per key**. A single human
  in an interactive chat sends a handful of requests per minute — nowhere near the caps.
- A paid key rides the exact same lane: point the record at the paid endpoint and the
  transport, discovery, and error handling are unchanged. Paid keys also lift the two
  free-tier weaknesses — rate limits and policy churn.
- We take on no inference cost, no key custody, no per-user billing — whether the user's
  key is free-tier or paid, the bill (if any) is between them and the provider.
- Trade-off: onboarding friction (the user must register + create a key) and, for
  free-tier keys, a hard dependency on third-party free-tier policy, which changes
  often. Mitigate by keeping the provider **pluggable** (see
  [the lane model](#the-lane-model)) — never hard-code one. The shared-key lane is the
  friction escape hatch when a trusted deployment exists.

### Key handling

The [guarantees](#guarantees-we-hold-ourselves-to) above are binding here; the rest of
this lane's rules:

1. **OpenAI-compatible first.** Prefer providers exposing the OpenAI Chat Completions
   shape so one client path serves all of them. Non-compatible providers need an adapter.
2. **Nothing hard-coded.** `base_url`, `model`, and provider are all user/region
   configurable. Free model IDs rotate and endpoints get deprecated.
3. **Degrade, don't crash.** A `429` (rate/quota) or model-removed error must surface a
   clear user message and, where configured, fall back to on-device or another provider.

### Providers

All three primary providers speak the OpenAI Chat Completions API. Endpoint + auth +
default model below; verify limits against each provider's live console before shipping —
these are 2026-07 snapshots and move.

| Provider | `base_url` | Auth header | Default model | Free tier (per key, snapshot) |
|---|---|---|---|---|
| **Google Gemini** | `https://generativelanguage.googleapis.com/v1beta/openai/` | `Authorization: Bearer <key>` | `gemini-3.6-flash` | 10 RPM / 250 RPD, 250K TPM, 1M ctx |
| **NVIDIA NIM** | `https://integrate.api.nvidia.com/v1` | `Authorization: Bearer <key>` | `deepseek-ai/deepseek-v4-flash` | ~40 RPM; large models cost more credits |
| **Zhipu GLM** (CN) | `https://open.bigmodel.cn/api/paas/v4` | `Authorization: Bearer <key>` | `glm-4.7-flash` | Permanently free, no total cap |

Secondary / optional (all OpenAI-compatible, useful as fallbacks):

| Provider | `base_url` | Default model | Note |
|---|---|---|---|
| **Groq** | `https://api.groq.com/openai/v1` | `llama-3.3-70b-versatile` | Fastest; ~30 RPM / 1000 RPD |
| **Cerebras** | `https://api.cerebras.ai/v1` | `gpt-oss-120b` | 1M tok/day but 5 RPM, 8K ctx |
| **OpenRouter** | `https://openrouter.ai/api/v1` | `<id>:free` | Aggregator; free IDs rotate — don't pin |
| **Mistral** | `https://api.mistral.ai/v1` | `mistral-small-latest` | ~2 RPM, eval-only |

> **Do not use GitHub Models** — retired 2026-07-30.

Where to register (link these in the in-app key-entry flow):

- Gemini → https://aistudio.google.com/apikey
- NVIDIA → https://build.nvidia.com (join the free Developer Program)
- Zhipu → https://open.bigmodel.cn (国内实名注册)

Region default: on first run, pick `cn` (Zhipu) for CN locale/region, else `global`
(Gemini). Always overridable.

**Paid keys.** The tables above are the *free-tier* defaults the app ships with;
nothing limits BYOK to them. A user with a paid account points a lane record at the
paid endpoint (`https://api.openai.com/v1`, DashScope, a topped-up Zhipu/NVIDIA
balance, …) — the editable `base_url`/`model` already allow this, and the transport is
identical. Providers whose native API is not OpenAI Chat-shaped (e.g. Anthropic) need
an `adapter:<name>` lane record rather than `openai-chat`.

**PHI and BAAs.** For a deployment handling PHI under HIPAA, none of the public consumer
endpoints above are BAA-covered. Either keep the turn on the local / home-server lane
(nothing leaves), or point the lane at a BAA-eligible enterprise surface — Google Gemini
via **Vertex AI** (`GOOGLE_CLOUD_PROJECT` + `GOOGLE_CLOUD_LOCATION`, credentials via ADC)
or OpenAI via **Azure OpenAI** (`AZURE_OPENAI_*`), both
already supported server-side. See [README → Compliance](../README.md#compliance--hipaa--gdpr).

### Request contract

Identical for every `openai-chat` lane — only `base_url`, `model`, and the key differ:

```
POST {base_url}/chat/completions
Authorization: Bearer {key}
Content-Type: application/json

{ "model": "{model}", "messages": [...], "stream": true }
```

Stream SSE `data:` lines; each chunk is `choices[0].delta.content`. `[DONE]` ends the
stream. This is the same shape a local Ollama/llama.cpp server exposes, which is why one
transport serves every HTTP lane, local or remote.

### Model discovery — don't hard-code model IDs

Never ship a hard-coded per-provider model list as the source of truth. Model IDs rotate,
get renamed, and get retired (a retired ID returns `404`/`410`). Worse, a provider's *docs*
can list a model that isn't actually deployed on the free hosted endpoint — so "it's in the
docs" is not proof it works on a given key.

**Query the live catalog instead:** `GET {base_url}/models` (OpenAI-compatible, auth same as
chat) returns `{ "data": [ { "id", "object", "created", "owned_by" }, … ] }` — the
authoritative list of what that key can actually call. Truly-retired models simply don't
appear, so this alone stops the 404/410 whack-a-mole. The built-in `model` in a lane
record is just a sensible default + validation target, not a menu.

#### Optional enrichment: join with the provider's website catalog

`/v1/models` is authoritative but thin — it has **no free/paid flag, no popularity, and (for
NVIDIA) a placeholder `created` that is identical for every model**, so don't sort by it. The
richer metadata lives in the *website* catalog, not the inference API. For NVIDIA it's the
`build.nvidia.com/models` page (add `&pageSize=96` + `filters=nimType:nim_type_preview`),
whose data ships as escaped JSON embedded in ~1 MB of HTML (a Next.js RSC payload) — there is
**no clean catalog JSON endpoint**. Parse it by splitting on `"resourceType":"ENDPOINT"` and
reading each record's attributes:

- `PREVIEW=true` → **Free Endpoint** (the 🆓 flag). `nimType`: `nim_type_preview`=free,
  `nim_type_run_anywhere`=downloadable, `nim_type_upgrade_available`=partner (paid) endpoint.
- `last_month_api_invocation_count` → real **call volume** (use for a popularity sort).
- `DEPRECATION` → a future **retire date** — surface as a "retires on …" warning.
- **`AVAILABLE` is NOT reliable** — it marks working models as `false`; never filter on it.

Join the two live sources by a **normalized name**: strip the publisher prefix, lowercase,
and fold `_` and `.` to `-` (catalog slug `llama-3_3-70b-instruct` ↔ API id
`meta/llama-3.3-70b-instruct`). If the website fetch/parse fails, degrade to the bare
`/v1/models` list — it's undocumented and may change without notice, so never hard-depend on it.

Billing note (NVIDIA free tier): no card bound = no charge. Free access is rate-limited
(~40 RPM); a few large models draw a signup credit pool; hitting a limit is a rejected request,
never a bill.

### Provider quirks (adapter notes)

Even "OpenAI-compatible" providers have per-model request quirks. Keep these in the transport
as small, documented adapters — not scattered at call sites:

- **NVIDIA DeepSeek V4 (flash & pro) are reasoning models that hang with empty output unless
  the request body carries `chat_template_kwargs` at the root.** Send `{ "thinking": false }`
  for a normal answer (with `thinking: true` the answer streams in `reasoning_content`, a
  separate field most UIs don't render — looks like "no response"). Match on `deepseek-v4*`.
- Reasoning models generally either emit `<think>…</think>` inline (Qwen) or a separate
  `reasoning_content` delta (DeepSeek) — the UI must decide to render, fold, or drop it.
  The same fold logic covers the local lane's Qwen models.
- 70B-dense models (e.g. `meta/llama-3.3-70b-instruct`) work but are slow on free tiers;
  prefer low-active-param MoE models (`deepseek-v4-flash`, `qwen3-next-80b-a3b-instruct`) for
  latency. `openai/gpt-oss-120b` is OpenAI's open-weight model, not a GPT-4/5 endpoint.

### Error handling

| Condition | HTTP | Action |
|---|---|---|
| Rate/quota exceeded | 429 | Show "provider busy/quota reached"; retry after backoff; offer fallback lane |
| Bad/expired key | 401/403 | Prompt user to re-enter key; open the provider's key page |
| Model removed/renamed | 404 | Reset to provider default model; warn user |
| Endpoint deprecated | 404/410 | Fall back to next configured provider |

Mid-turn failures feed back into the router's guard rails: the turn degrades to local
with a notice rather than dying.

---

# Part 2 — The storage tiers

## Backend *is* the storage selector

The app already has the control this needs: **Backend** in the nav drawer (Android's
`BaseUrlDialog` in [AppSettingsSection.kt](../android/app/src/main/java/ai/thetahealth/mirobody/ui/settings/AppSettingsSection.kt),
the base-URL override in [net.js](../htdoc/src/net.js)). It sets the base URL every
request is prefixed with — and since chat history, uploads, health records, and the
account all live behind that server, **Backend already decides where the user's data
lives.** No new concept is required; it just needs to be named as the privacy control it
is.

| Backend points at | Storage tier | Physically |
|---|---|---|
| *(nothing — this device)* | **local** | device SQLite (or DuckDB) + the device filesystem |
| a home server URL | **home-server** | that box's Postgres/SQLite + `LocalStorage` (or a self-run MinIO) |
| a hosted deployment | **cloud-shared** | the deployment's Postgres + S3 / OSS |

The tiers line up with the inference lanes one-for-one, with one simplification: there is
**no BYOS ("bring your own storage") client tier**. A user's own S3 bucket is configured
*server-side* (`S3_*` / `ALI_OSS_*` in [config.example.yml](../config.example.yml)), so
"I want my data in my own bucket" is already covered by *running your own backend* — it
collapses into the home-server tier instead of adding a fourth. The storage axis is
genuinely simpler than the inference axis: three tiers, one setting.

## What is stored, and where each piece goes

Two stores, both behind interfaces that all three tiers share:

- **SQL** — one `Database` class with the backend linked at build time
  ([src/database/README.md](../src/database/README.md)). Mobile builds are restricted to
  `SQLITE` / `DUCKDB` precisely because a device holds **one user's data and the app is
  the only writer**; server builds default to `POSTGRESQL`. Holds accounts, chat
  (`conversations` / `messages`), the uploads index (`files`), health links, FHIR
  resources, and memories.
- **Objects** — one `Storage` interface with four interchangeable backends
  ([src/storage/README.md](../src/storage/README.md)): `AwsS3` (also every S3-compatible
  cloud — R2, B2, MinIO, Spaces, COS, OBS, GCS via its XML API), `AliyunOss`,
  `AzureBlob`, and `LocalStorage`. All four compile into every build and the tier is a
  **runtime** config choice. `LocalStorage` is the no-credentials fallback that makes the
  local and home-server tiers work with zero cloud setup.

Privacy-relevant properties that hold in every tier, because they live in the shared
layer rather than a per-tier branch:

- **Per-user prefixes are hashed.** An object key is
  `<hashed user seg>/<shard>/<hashed digest>[.ext]`, both segments keyed HMACs — so a
  path never carries the sequential account id, and identical bytes still dedupe to one
  object (content-addressed).
- **Uploads carry a `.meta` sidecar** (JSON Lines: content type, size, timestamp,
  original filename) and deliberately **no user id**.
- **Vendor OAuth tokens are stored encrypted** ([src/health/README.md](../src/health/README.md)),
  and health access is ownership-gated before any record can be read.

## Encryption at rest — and the E2E question

With `FILE_ENCRYPTION_KEY` set, user uploads and their `.meta` / `.trans` sidecars are
stored **Fernet-encrypted**, so a leaked S3/OSS/local credential yields ciphertext rather
than lab reports. Two secrets, deliberately separate, which is what makes rotation cheap:

- **`FILE_ENCRYPTION_KEY` is a list** — the last key encrypts new writes, all keys are
  tried on read. Rotate by appending, then drop the old key once nothing needs it (the
  `file_rekey` CLI rewrites objects under the newest key).
- **`FILE_KEY_SEED` is a single stable secret** that seeds object-key derivation, kept
  apart from the ciphers so a cipher rotation leaves every path unchanged. It must never
  change once uploads exist.

The server records non-secret fingerprints in a `.file-encryption-check` marker and
**refuses to start** if the seed changed or the key list shares nothing with the recorded
set — an accidental config change fails loudly instead of silently orphaning data. Read
paths flow through the app's decrypting mount (`signed_read_url`), since a client can't
decrypt bucket bytes itself.

Keying object paths with the seed (not the public bucket name) also closes the
*confirmation-of-file* exposure: a credential holder cannot recompute a user's object
keys, so they can't even answer "does this user have this file?"

**The open design question is who holds the key.** Today it is server config, which
protects against a storage-credential leak but not against the server operator. Moving
the key into the device secure store — the same place BYOK provider keys live — would
make the hosted tier end-to-end encrypted and bring its privacy close to the home-server
tier. The cost is unforgiving: lose the key, lose the data, with no operator-side
recovery. For health records that trade needs an explicit, well-understood opt-in rather
than a default, plus a recovery story (printed recovery code? per-household escrow?).

## Placement is a policy, not a route

The inference router picks a lane per turn; storage cannot work that way, and the
document should not pretend otherwise:

- **One source of truth.** Whatever Backend points at is authoritative. There is no
  "fall back to the other store" — that would fork the data.
- **Switching Backend is not a migration.** Point the app at a different server and you
  see what *that* server holds. This is honest semantics (placement is a setting), but it
  means changing tiers needs an explicit export/import or sync step.
- **The gap: there is no sync protocol yet.** Device SQLite and server Postgres share the
  DDL shape but nothing reconciles them; the live multi-turn window is cache-backed and
  only the opening question of a conversation is persisted so far. Offline edits,
  multi-device convergence, and conflict resolution are unbuilt — the single largest piece
  of work on this axis, and the reason the local tier is currently "this device only, no
  backup" rather than "local-first with replication".

Both user-facing consequences of this — the offline tier has no backup, and the
home-server tier's privacy is household-level — are the caveats stated up front in
[What actually leaves the device](#what-actually-leaves-the-device-per-artifact).

---

# Where the two lines meet

The axes are independent, but they are not unrelated — three couplings matter:

1. **Retrieval binds them.** A useful health answer needs the user's records as context.
   Where the data lives therefore constrains where a turn can go: injecting health
   history into a cloud turn sends that history to the provider. This is why the router
   carries `context_is_health` and why the escalation notice should name *what* is
   leaving, not just *where* it goes.
2. **The one-box optimum.** A home server holding both the 32B model and the household's
   data is the only configuration where retrieval-augmented health answers happen with
   **nothing** leaving the LAN. That is the configuration to recommend to a
   privacy-motivated user who wants real answer quality.
3. **Their defaults differ, for good reasons.** Inference defaults local because remote
   buys *capability* the user may not need. Storage arguably should not default to
   device-only forever, because remote buys *durability* every user needs — a
   local-first-plus-encrypted-replica default is the likely end state once sync exists.

## Open questions

**Routing**

- Threshold values for the heuristic (token count, which keywords) — tune on real data.
- Where local self-assessment gets its confidence signal (logprobs? a follow-up "are you
  sure" pass?) when we add it.
- Whether a turn can be *split* (local drafts, cloud refines) or is strictly one lane.
- Router state: per-turn only, or sticky within a session once escalated?
- How `context_is_health` is decided — tagged at retrieval time, or classified?

**Cloud lanes**

- Streaming edge cases per provider (some emit usage in a trailing chunk).
- ToS review for shipping a BYOK app at scale (individual keys are fine; aggregating a
  large user base against one provider's free tier may not be — another argument for the
  shared-key lane, where quota belongs to the deployment).
- Shared-key lane auth: how the client authenticates to the trusted server (account
  token?) and whether the server does its own tiering behind `model: "auto"`.

**Home server (both axes)**

- Operability: LAN discovery (mDNS?), remote access when away (VPN / Tailscale + TLS),
  token issuance/rotation inside the household, model management on the box, KV-cache
  budget per concurrent user.
- Whether a home server may itself hold a provider key and do second-stage escalation
  (32B locally, forward the hardest turns to the cloud) — that would collapse
  home-server and cloud-shared into one endpoint from the client's view.
- Per-person privacy inside a shared box: separate accounts are enough for access
  control, but the admin still holds the disk. Is per-user encryption worth it at home?

**Storage**

- Sync protocol between device SQLite and a server DB: change feed, conflict resolution,
  and whether health records (append-mostly) and chat (append-only) can share one design.
- Export / import when a user switches Backend — and whether "move my data to my new home
  server" should be a first-class flow.
- Client-held `FILE_ENCRYPTION_KEY` (true E2E for the hosted tier) plus a recovery story
  that doesn't hand the operator a backdoor.
- A **Backend = "this device"** option in the settings menu, so the local tier is an
  explicit user choice rather than an implicit consequence of not configuring a server
  (HarmonyOS currently omits the Backend item entirely).
- Whether the UI should show the current tier pair as a single "privacy posture"
  indicator, rather than leaving the user to infer it from two separate settings screens.
