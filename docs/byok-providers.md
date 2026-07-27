# BYOK Providers — Cloud LLM Contract

**BYOK = Bring Your Own Key.** The app ships as a client only. Each end user pastes
their *own* API key for a cloud LLM provider; inference runs on that provider under
the user's own free-tier quota. We host no inference and consume no quota of our own.

This is the cloud-side complement to [on-device-llm.md](on-device-llm.md): same product
(one private chat), different execution lane. On-device runs the model locally; BYOK
runs it in the cloud on the user's key. A device may offer both and let the user choose.

How the app decides *which* lane a given turn takes — local 4B by default, cloud for the
hard ones — is defined one level up in [tiered-inference.md](tiered-inference.md). This
doc only covers the cloud lane's provider contract.

## Why BYOK works here

- The per-user free quotas below (e.g. 250 requests/day) are **per key**. A single human
  in an interactive chat sends a handful of requests per minute — nowhere near the caps.
- We take on no inference cost, no key custody, no per-user billing.
- Trade-off: onboarding friction (the user must register + create a key) and a hard
  dependency on third-party free-tier policy, which changes often. Mitigate by keeping
  the provider **pluggable** (see [Config schema](#config-schema)) — never hard-code one.

## Hard rules

1. **Keys live on-device only.** Store in the platform secure store (Keychain / Keystore /
   Windows Credential Manager). **Never** send a user key to any Mirobody server, log it,
   or embed it in telemetry/crash reports.
2. **OpenAI-compatible first.** Prefer providers exposing the OpenAI Chat Completions
   shape so one client path serves all of them. Non-compatible providers need an adapter.
3. **Nothing hard-coded.** `base_url`, `model`, and provider are all user/region
   configurable. Free model IDs rotate and endpoints get deprecated.
4. **Degrade, don't crash.** A `429` (rate/quota) or model-removed error must surface a
   clear user message and, where configured, fall back to on-device or another provider.

## Providers

All three primary providers speak the OpenAI Chat Completions API. Endpoint + auth +
default model below; verify limits against each provider's live console before shipping —
these are 2026-07 snapshots and move.

| Provider | `base_url` | Auth header | Default model | Free tier (per key, snapshot) |
|---|---|---|---|---|
| **Google Gemini** | `https://generativelanguage.googleapis.com/v1beta/openai/` | `Authorization: Bearer <key>` | `gemini-2.5-flash` | 10 RPM / 250 RPD, 250K TPM, 1M ctx |
| **NVIDIA NIM** | `https://integrate.api.nvidia.com/v1` | `Authorization: Bearer <key>` | `deepseek-ai/deepseek-v4-pro` | ~40 RPM; large models cost more credits |
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

## Config schema

A provider is fully described by this record; the app ships a built-in list and lets the
user pick one + paste a key. `base_url` and `model` stay editable so power users can
point at any OpenAI-compatible endpoint.

```jsonc
{
  "id": "nvidia",                // stable internal id
  "label": "NVIDIA NIM",         // shown in UI
  "base_url": "https://integrate.api.nvidia.com/v1",
  "model": "deepseek-ai/deepseek-v4-pro",
  "auth_scheme": "bearer",       // bearer | header:<name> | query:<name>
  "region_hint": "global",       // global | cn — for default selection
  "compatible": "openai-chat"    // openai-chat | adapter:<name>
}
```

Active selection persisted per user:

```jsonc
{ "provider_id": "nvidia", "model": "deepseek-ai/deepseek-v4-pro" }
// key is NOT here — it lives in the secure store, addressed by provider_id.
```

Region default: on first run, pick `cn` (Zhipu) for CN locale/region, else `global`
(Gemini). Always overridable.

## Request contract

Identical for every `openai-chat` provider — only `base_url`, `model`, and the key differ:

```
POST {base_url}/chat/completions
Authorization: Bearer {key}
Content-Type: application/json

{ "model": "{model}", "messages": [...], "stream": true }
```

Stream SSE `data:` lines; each chunk is `choices[0].delta.content`. `[DONE]` ends the
stream. This is the same shape a local Ollama/llama.cpp server exposes, so the on-device
and BYOK lanes can share one transport.

## Model discovery — don't hard-code model IDs

Never ship a hard-coded per-provider model list as the source of truth. Model IDs rotate,
get renamed, and get retired (a retired ID returns `404`/`410`). Worse, a provider's *docs*
can list a model that isn't actually deployed on the free hosted endpoint — so "it's in the
docs" is not proof it works on a given key.

**Query the live catalog instead:** `GET {base_url}/models` (OpenAI-compatible, auth same as
chat) returns `{ "data": [ { "id", "object", "created", "owned_by" }, … ] }` — the
authoritative list of what that key can actually call. Truly-retired models simply don't
appear, so this alone stops the 404/410 whack-a-mole. The built-in `model` in a provider
record is just a sensible default + validation target, not a menu.

### Optional enrichment: join with the provider's website catalog

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

## Provider quirks (adapter notes)

Even "OpenAI-compatible" providers have per-model request quirks. Keep these in the transport
as small, documented adapters — not scattered at call sites:

- **NVIDIA DeepSeek V4 (flash & pro) are reasoning models that hang with empty output unless
  the request body carries `chat_template_kwargs` at the root.** Send `{ "thinking": false }`
  for a normal answer (with `thinking: true` the answer streams in `reasoning_content`, a
  separate field most UIs don't render — looks like "no response"). Match on `deepseek-v4*`.
- Reasoning models generally either emit `<think>…</think>` inline (Qwen) or a separate
  `reasoning_content` delta (DeepSeek) — the UI must decide to render, fold, or drop it.
- 70B-dense models (e.g. `meta/llama-3.3-70b-instruct`) work but are slow on free tiers;
  prefer low-active-param MoE models (`deepseek-v4-flash`, `qwen3-next-80b-a3b-instruct`) for
  latency. `openai/gpt-oss-120b` is OpenAI's open-weight model, not a GPT-4/5 endpoint.

## Error handling

| Condition | HTTP | Action |
|---|---|---|
| Rate/quota exceeded | 429 | Show "provider busy/quota reached"; retry after backoff; offer fallback lane |
| Bad/expired key | 401/403 | Prompt user to re-enter key; open the provider's key page |
| Model removed/renamed | 404 | Reset to provider default model; warn user |
| Endpoint deprecated | 404/410 | Fall back to next configured provider |

## Open questions

- Streaming edge cases per provider (some emit usage in a trailing chunk).
- Whether to bundle a shared transport with the local Ollama path (recommended) or keep
  separate — decide when the first platform lands.
- ToS review for shipping a BYOK app at scale (individual keys are fine; aggregating a
  large user base against one provider's free tier may not be).
- Which platform lands first (mobile / desktop / harmony / android).
