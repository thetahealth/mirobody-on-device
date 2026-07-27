# Tiered Inference — Local-First with Cloud Escalation

One chat, two execution lanes, one router deciding between them per turn:

- **Local (default):** the on-device 4B model — see [on-device-llm.md](on-device-llm.md).
  Low latency, offline, zero cost, data never leaves the device.
- **Cloud (escalation):** a BYOK cloud provider on the user's own key — see
  [byok-providers.md](byok-providers.md). Used only when the local model isn't enough.

This document sits above both: it defines *how a turn is routed* between them. The two
lane docs define *how each lane talks to a model*; both expose the same OpenAI
Chat/SSE contract, so everything above the router is lane-agnostic.

## Why tiered, not either/or

| | Local 4B | Cloud (BYOK) |
|---|---|---|
| Everyday Q&A, rewrite, summarize, chat | ✅ ideal | overkill |
| Long context, multi-step reasoning, code, fresh knowledge | ✗ struggles | ✅ |
| Latency | instant | network-bound |
| Cost to us / user | zero | zero (user's free key) |
| Privacy | data stays on device | **data leaves the device** |
| Offline | ✅ | ✗ |

Default to local for privacy, latency, and offline. Escalate only when the task needs it —
and escalation crosses a privacy boundary, so it is never silent (see [UX](#escalation-ux)).

## The pipeline

A turn flows through one pipeline; the **router** is a single replaceable stage that
picks the lane. Everything downstream is identical regardless of choice.

```
turn ─▶ router.select(turn, ctx) ─▶ { lane, provider? } ─▶ transport(OpenAI Chat/SSE) ─▶ stream
                                          │
                          local ──────────┘
                          cloud (byok) ───┘
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
    "offline": false,
    "byok_configured": true     // is a cloud key present?
  }
}
// output
{
  "lane": "local",            // "local" | "cloud"
  "provider_id": null,        // set when lane == "cloud" (see byok-providers.md)
  "reason": "default"         // for telemetry + the escalation notice
}
```

Guard rails independent of strategy:

- `offline` or `byok_configured == false` → force **local** (cloud isn't reachable).
- `user_forced_lane` always wins.
- Cloud unreachable / `429` / key invalid mid-turn → fall back to local with a notice.

## Escalation UX

Escalation moves the conversation off-device, so it must be visible and, ideally,
consented:

- When routing to cloud, tell the user plainly: *"Handing this to the cloud model — it
  will use your <provider> key and this message leaves your device."*
- Offer a "keep it local" opt-out on that turn.
- Make the default lane and the deep-thinking toggle discoverable, not buried.
- Never escalate health-sensitive content without the user having opted into cloud at
  least once.

## Open questions

- Threshold values for the heuristic (token count, which keywords) — tune on real data.
- Where local self-assessment gets its confidence signal (logprobs? a follow-up "are you
  sure" pass?) when we add it.
- Whether a turn can be *split* (local drafts, cloud refines) or is strictly one lane.
- Router state: per-turn only, or sticky within a session once escalated?
