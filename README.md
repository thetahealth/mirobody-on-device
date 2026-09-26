<div align="center">

# mirobody-on-device

**The phone runtime of mirobody: your health record, and the model that answers over it, stay on the phone.**

**English** · **[中文](README.zh-CN.md)**

[![License: Apache-2.0](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![C++11](https://img.shields.io/badge/C%2B%2B-11-00599C.svg?logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![CI](https://github.com/thetahealth/mirobody-on-device/actions/workflows/ci.yml/badge.svg)](https://github.com/thetahealth/mirobody-on-device/actions/workflows/ci.yml)
[![Platforms](https://img.shields.io/badge/platforms-Android%20·%20iOS%20·%20HarmonyOS-lightgrey.svg)](#build-the-apps)

**[mirobody, the server](https://github.com/thetahealth/mirobody)** · **[📚 Documentation](https://docs.mirobody.ai/)** · **[mirobody-web](https://github.com/thetahealth/mirobody-web)**

</div>

---

A watch syncs steps and heart rate to your phone, a ring writes sleep, a cuff
writes blood pressure. The phone ends up holding more of your health than any
server does, and it is the one place that data never has to leave.
mirobody-on-device is a C++ core that runs *inside* the Android, iOS and
HarmonyOS apps. It reads what only a phone can reach, codes each reading to
LOINC and UCUM, keeps it in a local SQLite record, and answers questions over
that record with a model running on the device. Point the app at a
[mirobody](https://github.com/thetahealth/mirobody) server and it works as that
server's client instead.

<p align="center">
  <img src="docs/images/what-is-mirobody.svg" alt="What is Mirobody? One health AI that runs anywhere and keeps your data yours: on a server (self-hosted, the whole family), on your phone (just you, works offline), or peer-to-peer." width="920">
</p>

## What it does

- **Collects what only a phone reaches.** Apple HealthKit, Android Health
  Connect, Huawei Health, and standard Bluetooth health sensors (GATT /
  IEEE-11073) on Android and iOS.
- **Codes it the way the server does.** Each reading gets a LOINC code for what
  was measured and a UCUM unit, and is stored as a FHIR `Observation`. The
  original upload is kept as a file. The coding tables live in each app today.
  They are moving to the device vocabulary the main repo publishes, so the phone
  and the server cannot disagree about what a field means.
- **Answers offline.** A local agent loop calls the same MCP-style tools over
  the local record, driven by a model on the device: LiteRT-LM or llama.cpp,
  Gemma-class, 1–4B parameters at 4-bit.
- **Or your own key, straight to the provider.** Bring your own key (BYOK) and
  the model is in the cloud, but the record stays on the phone.
- **One C ABI, three apps.** The apps drive the core through the 24 functions
  in [`src/mirobody.h`](src/mirobody.h). There is no second implementation per
  platform.

<p align="center">
  <img src="docs/images/where-your-data-comes-from.svg" alt="Where your data comes from: wearables, phone health, lab results, clinic records, and everyday photo or voice logging all flow into mirobody, which normalizes everything to FHIR R4, then a model answers in plain language." width="920">
</p>

## Try it in two minutes

The core builds and runs on a Linux or macOS desktop, so you can try it without
a phone:

```sh
# macOS (Linux: the apt line is in docs/BUILDING.md)
brew install cmake ninja pkg-config libwebsockets openssl@3 rapidjson yaml-cpp \
             sqlite jpeg-turbo libpng libtiff webp catch2

git clone https://github.com/thetahealth/mirobody-on-device.git && cd mirobody-on-device
./build.sh                    # core, loopback server, CLIs, tests -> build/
build/tests/mirobody_tests    # the unit tests
build/fhir normalize "5.62 mmol/L" "72 bpm"
```

```json
{"input":"5.62 mmol/L","comparator":"","value":5.62,"unit":"mmol/L","family":"SCnc"}
{"input":"72 bpm","comparator":"","value":72.0,"unit":"/min","family":"NRat"}
```

`bpm` is not a UCUM unit: it comes out as `/min`, with the LOINC property
family (`NRat`, a number rate) that decides which codes it can sit under. See
what the on-device agent is allowed to call with `build/mcp list`. Start the
loopback server the apps talk to with `build/mirobody`; it reads
[`config.example.yml`](config.example.yml), listens on `127.0.0.1:8080` and
nowhere else, and `curl 127.0.0.1:8080/api/health` answers `ok`.

## How it fits with mirobody

| Concern | Lives in |
|---|---|
| The server: accounts, care circle, Postgres, Garmin / Oura / Whoop pulls, file extraction, the agent, MCP, export / import | [mirobody](https://github.com/thetahealth/mirobody) |
| Terminology (LOINC / UCUM / ICPC-3 / device crosswalks), the one source of truth | mirobody, published as data this repo consumes |
| The API contract, the SSE wire format, `/mirobody.json` capabilities | mirobody ([docs.mirobody.ai](https://docs.mirobody.ai/)) |
| The web UI | [mirobody-web](https://github.com/thetahealth/mirobody-web) |
| Local deployment on a desktop | mirobody (Docker, and the local-model profile) |
| **Phone apps, native collectors, the offline core, the C ABI** | **this repo** |

A feature that a server could run belongs in the main repo. What lands here is
what needs the phone: its health stores, its sensors, its offline model, and
its sandbox.

## Privacy

Two independent choices, and both default to the device:

| Lane | Model | Record |
|---|---|---|
| **On-device** | runs on the phone | on the phone; nothing leaves it |
| **BYOK** | the user's own key, straight to the provider | on the phone; the turn (and any health context in it) reaches the provider |
| **mirobody server** | whatever that server is configured with | on that server (self-hosted or hosted) |

The loopback server binds `127.0.0.1` only, even when a caller asks for
`0.0.0.0`: the core holds a health record and is not meant to face a network.
The full matrix, what leaves the device per artifact, and the honest caveats
are in [docs/privacy-tiers.md](docs/privacy-tiers.md). Before relying on any of
it, read [SECURITY.md](SECURITY.md).

mirobody organizes and explains your own health data. It does not diagnose,
prescribe, or replace a clinician.

<p align="center">
  <img src="docs/images/on-device-llm.svg" alt="On-device LLM: one private chat model on the hardware you already own, with no server round-trip, quantized to fit device memory, GPU-accelerated, and swappable." width="920">
</p>

## Build the apps

| App | How it links the core | Guide |
|---|---|---|
| Android | `libmirobody.so` over JNI | [android/](android/README.md) |
| iOS | `mirobody.xcframework`, a static library | [ios/](ios/README.md) |
| HarmonyOS | NAPI, built without the HTTP front door (`MIROBODY_MOBILE`) | [harmony/](harmony/README.md) |

Each app needs its dependencies cross-compiled once (the prebuilt sysroots);
[docs/BUILDING.md](docs/BUILDING.md) covers every target, including Windows
(`build.cmd` with vcpkg). `./build.sh mobile` builds the HarmonyOS profile on a
desktop host, which is the closest a desktop gets to a phone build: the core
static library is 3.8 MiB there, 5.4 MiB in the development profile
(macOS arm64). Runtimes, formats, quantization and measured phone numbers for
the on-device model are in [docs/on-device-llm.md](docs/on-device-llm.md).

## Layout

```
src/                  the C++ core (C++11)
  mirobody.h          the public C ABI: the embedding surface
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
tools/                repo maintenance scripts
docs/                 build guide, privacy tiers, on-device LLM, markdown spec
```

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
tag. What changed, release by release, is in the [CHANGELOG](CHANGELOG.md).

## 🤝 Contributing

The most useful report is a reading the phone coded wrong: a HealthKit, Health
Connect or Huawei Health field that landed under the wrong code or unit, or
under none.
[Report it](https://github.com/thetahealth/mirobody-on-device/issues/new?template=wrong-reading.yml)
with the field name as the health store names it. The gates a pull request must
pass are the same ones CI runs:

```sh
./build.sh && build/tests/mirobody_tests && ./build.sh mobile
python3 tools/check_doc_links.py && python3 tools/check_exports.py
```

→ [CONTRIBUTING.md](CONTRIBUTING.md) · [AGENTS.md](AGENTS.md) (for coding
agents) · [SECURITY.md](SECURITY.md) · [Code of Conduct](CODE_OF_CONDUCT.md) ·
[CHANGELOG](CHANGELOG.md)

## 📚 Documentation

[`docs/`](docs/README.md) holds the build guide, the privacy tiers, the
on-device model deck and the rendering spec every client answers to; most
directories under `src/` have a README for their subsystem. The server, the API and
the terminology are documented at **[docs.mirobody.ai](https://docs.mirobody.ai/)**.

<div align="center">

Apache 2.0 · © 2026 [Theta Health](https://thetahealth.ai)

</div>
