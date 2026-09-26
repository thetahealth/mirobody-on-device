# mirobody-on-device

**The phone runtime of [mirobody](https://github.com/thetahealth/mirobody).**
A C++ core that runs *inside* the Android, iOS and HarmonyOS apps, so a person's
health record, and the model that answers questions about it, can stay on the
phone.

<p align="center">
  <img src="docs/images/what-is-mirobody.svg" alt="What is Mirobody? One health AI that runs anywhere and keeps your data yours: on a server (self-hosted, the whole family), on your phone (just you, works offline), or peer-to-peer." width="920">
</p>

mirobody takes health information from any source, settles it into one standard
(LOINC for what was measured, UCUM for the unit) and answers over that record,
every number citing where it came from. The [main repo](https://github.com/thetahealth/mirobody)
is the server: self-hosted with Docker, multi-user, the family's shared record.
This repo is everything a Python server cannot be: the part that lives on the
phone.

It does three things:

- **Collect** what only a phone can reach: Apple HealthKit, Android Health
  Connect, Huawei Health, and standard Bluetooth health sensors (GATT /
  IEEE-11073). Readings are coded to LOINC and stored as FHIR `Observation`s, with
  the original uploads kept as files. (The coding tables currently live in each
  app; they are moving to the device crosswalk the main repo publishes.)
- **Answer offline.** A local agent loop with the same MCP-style tools, over a
  local SQLite record, driven by a model on the device (LiteRT-LM or llama.cpp,
  Gemma-class, 1–4B at 4-bit). Or the user's own API key, straight to the
  provider, with the record still on the phone.
- **Connect to a mirobody server** when the user points the app at one. Today
  the apps talk to a server the same way they talk to the core; syncing the
  on-device record to it, and importing / exporting the same FHIR Bundle the
  server does, is the next step (see [Where this is going](#where-this-is-going)).

<p align="center">
  <img src="docs/images/where-your-data-comes-from.svg" alt="Where your data comes from: wearables, phone health, lab results, clinic records, and everyday photo or voice logging all flow into mirobody, which normalizes everything to FHIR R4, then a model answers in plain language." width="920">
</p>

## How the three repos divide the work

| Concern | Lives in |
|---|---|
| The server: accounts, care circle, Postgres, Garmin / Oura / Whoop pulls, file extraction, the agent, MCP, export / import | [mirobody](https://github.com/thetahealth/mirobody) |
| Terminology (LOINC / UCUM / ICPC-3 / device crosswalks), the one source of truth | mirobody, published as data this repo consumes |
| The API contract, the SSE wire format, `/mirobody.json` capabilities | mirobody ([docs.mirobody.ai](https://docs.mirobody.ai/)) |
| The web UI | [mirobody-web](https://github.com/thetahealth/mirobody-web) |
| Local deployment on a desktop | mirobody (Docker, and the local-model profile) |
| **Phone apps, native collectors, the offline core, the C ABI** | **this repo** |

## Where the model runs, where the data lives

Two independent choices, both defaulting to the device:

| Lane | Model | Record |
|---|---|---|
| **On-device** | runs on the phone | on the phone; nothing leaves it |
| **BYOK** | the user's own key, straight to the provider | on the phone; the turn (and any health context in it) reaches the provider |
| **mirobody server** | whatever that server is configured with | on that server (self-hosted or hosted) |

The full matrix, what leaves the device per artifact, and the honest caveats are
in [docs/privacy-tiers.md](docs/privacy-tiers.md).

<p align="center">
  <img src="docs/images/on-device-llm.svg" alt="On-device LLM: one private chat model on the hardware you already own, with no server round-trip, quantized to fit device memory, GPU-accelerated, and swappable." width="920">
</p>

Runtimes, formats, quantization and measured phone numbers are in the slide deck
[docs/on-device-llm.md](docs/on-device-llm.md).

## What mirobody does not do

It organizes and explains your own health data. It does not diagnose, prescribe,
or replace a clinician.

## Layout

```
src/                  the C++ core (C++11)
  mirobody.h          the public C ABI -- the embedding surface
  platform/           the JNI (Android) and iOS bridges, the C ABI implementation
  chat/  llm/  mcp/   the agent loop, streaming model clients, the tool registry
  fhir/  indicator/   the FHIR store and write path, the terminology resolver
  health/             on-device ingest (health-store batches -> FHIR)
  database/ storage/  SQLite and the local file store
  server/             the loopback HTTP front door (Android / iOS use it today;
                      the HarmonyOS profile builds without it)
res/                  agents, MCP tools, the SQLite schema, terminology artifacts
android/ ios/ harmony/  the three host apps
tests/                C++ unit tests
cli/                  per-subsystem debug tools
docs/                 build guide, privacy tiers, on-device LLM, markdown spec
```

## Build

```sh
./build.sh            # Linux / macOS: the core, loopback server, CLIs and tests -> build/
build/tests/mirobody_tests
./build.sh mobile     # the HarmonyOS profile (no HTTP front door) -> build-mobile/
```

Windows uses `build.cmd` with vcpkg. The app builds, their prebuilt sysroots and
the on-device model engines are covered per platform in
[docs/BUILDING.md](docs/BUILDING.md), [android/](android/README.md),
[ios/](ios/README.md) and [harmony/](harmony/README.md).

The C ABI in [src/mirobody.h](src/mirobody.h) is how the apps drive the core:
`mirobody_chat_messages` runs a turn and streams its events back through a
callback, `mirobody_health_store` / `mirobody_health_recent` write and read the
on-device record, and `mirobody_llm_*` loads and runs a local model.

## Where this is going

This repo was the full C++ port of an earlier mirobody server ("mirobody v2").
In September 2026 it narrowed to the phone, and it is being aligned with the main
repo in steps:

1. **Cut** what only a server needs. Done for the desktop and web clients, the
   server databases and object stores, Redis, the vendor-cloud connectors, the
   hosted memory services and the realtime voice lanes. The account, care-circle
   and OAuth layers go next, together with the apps' switch to a per-launch
   token, after which the HTTP front door becomes an opt-in module.
2. **One contract.** Speak the main repo's API and SSE wire exactly, answer the
   same capability document, and put the mirobody-web build in the apps' WebView
   so the phone and the server share one UI.
3. **One vocabulary.** Load the terminology the main repo publishes instead of
   building a separate lexicon.
4. **One record format.** Sync to a mirobody server, and export / import the same
   FHIR Bundle.

Everything removed along the way is preserved at the
[`v2-full-2026-08`](https://github.com/thetahealth/mirobody-on-device/tree/v2-full-2026-08)
tag.

## License

Apache License 2.0. See [LICENSE](LICENSE).
