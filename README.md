<div align="center">

# mirobody-on-device

**A local-first phone runtime for mirobody: collect, normalize, store and ask over your health data on the device you carry.**

**English** · **[中文](README.zh-CN.md)**

[![License: Apache-2.0](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg?logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![CI](https://github.com/thetahealth/mirobody-on-device/actions/workflows/ci.yml/badge.svg)](https://github.com/thetahealth/mirobody-on-device/actions/workflows/ci.yml)
[![Platforms](https://img.shields.io/badge/platforms-Android%20·%20iOS%20·%20HarmonyOS-lightgrey.svg)](#host-apps)

**[mirobody server](https://github.com/thetahealth/mirobody)** · **[Documentation](https://docs.mirobody.ai/)** · **[mirobody-web](https://github.com/thetahealth/mirobody-web)**

</div>

---

## Why this repository exists

Your phone is where health data from Apple Health, Health Connect, Huawei Health,
Bluetooth devices and files can meet. It can also be the one place where a
private health record and a small language model work without a network round
trip when the embedded core and model are configured.

`mirobody-on-device` contains the native runtime and the Android, iOS and
HarmonyOS host apps. HarmonyOS embeds the core; Android and iOS can embed it
when native dependencies are supplied. The intended shared data path is:

```text
phone health stores / BLE / files
              │
              ▼
      collect → map fields → FHIR Observation
              │                 │
              │                 └── SQLite + local files
              ▼
      local agent/tools or offline model → answer
```

The runtime can also use a user supplied provider key or connect to a mirobody
server. Those lanes have different data boundaries. Current defaults vary:
Android points to a test server unless overridden at build time or in settings; iOS points to `localhost:8080` but
ships without the native framework by default; HarmonyOS uses its embedded
core. Check the selected backend before uploading health data.

> **Product boundary:** mirobody organizes and explains a person's own health
data. It does not diagnose, prescribe or replace a clinician.

<p align="center">
  <img src="docs/images/what-is-mirobody.svg" alt="Mirobody can run on a self-hosted server or on the phone, while the user keeps control of the record." width="920">
</p>

## What the phone runtime does

- **Collects phone-only data.** The host apps provide HealthKit, Health Connect,
  Huawei Health and supported GATT / IEEE-11073 device adapters.
- **Codes phone measurements.** Host adapters currently map supported fields
  to LOINC concepts and UCUM units, then submit FHIR `Observation` resources.
  The main repo owns the terminology and device crosswalks; loading its
  versioned device bundle in this core is the next step.
- **Keeps a local record when embedded.** SQLite stores structured
  observations and app-managed files hold source documents. Model weights may
  live in app storage or a user-selected location, depending on the host.
  Android and iOS health sync follows the selected backend.
- **Supports offline inference.** The apps can run local models through
  llama.cpp or LiteRT-LM. The embedded agent can query local records through
  MCP-style tools. Connecting those tools to every app's offline model path
  remains work in progress.
- **Supports BYOK on the local core.** A provider sees messages, health
  context and tool results included in a cloud turn. Local storage does not
  imply local inference.
- **Shares a native boundary.** Android has JNI integration, iOS can link an
  XCFramework, and HarmonyOS uses NAPI. The public C ABI is in
  [`src/mirobody.h`](src/mirobody.h); Android and iOS still use a loopback
  compatibility path for most app operations.

<p align="center">
  <img src="docs/images/where-your-data-comes-from.svg" alt="Health data from phone stores, devices and files flows into a local FHIR record before a model answers." width="920">
</p>

## Where it fits

| Responsibility | Repository |
|---|---|
| Accounts, care circle, multi-user access, Postgres, object storage and vendor-cloud collection | [mirobody](https://github.com/thetahealth/mirobody) |
| Terminology build, LOINC / UCUM / ICPC-3 data and device crosswalks | [mirobody](https://github.com/thetahealth/mirobody) |
| API contract, SSE events, capability document and server deployment | [mirobody](https://github.com/thetahealth/mirobody) |
| Shared web UI | [mirobody-web](https://github.com/thetahealth/mirobody-web) |
| Phone health stores, sensors, local record, local model lane and C ABI | **this repository** |

A feature that only needs a server belongs in the main repo. A feature that
needs a phone permission, a sensor, an app sandbox or offline execution belongs
here. Today the apps can call a configured server and write supported FHIR
Observations. Consuming the upstream device vocabulary and sharing a FHIR
Bundle import/export format are planned integrations.

## Privacy lanes

The storage location and the model destination are separate concepts.
The table describes data flow when a lane is selected, not a uniform
first-launch default across all three apps:

| Lane | Model destination | Record destination |
|---|---|---|
| **On-device** | The phone | The phone's SQLite database and sandbox when the embedded core is the selected backend |
| **BYOK** | The provider selected by the user | The record follows the selected backend; the provider receives messages and any context or tool results included in the turn |
| **mirobody server** | The configured server lane | Health uploads and chat go to that server, self-hosted or hosted |

The checked-in development config binds to `127.0.0.1`; Android and iOS
embedded bridges force their listener to loopback even if config names a LAN address. The standalone
server currently accepts an explicit `HTTP_HOST` override. Loopback does not
authenticate other apps on the same phone. Per-launch authentication remains
a planned security improvement. The full artifact-by-artifact data flow is in
[privacy tiers](docs/privacy-tiers.md); the security assumptions and reporting
process are in [SECURITY.md](SECURITY.md).

## Try the core on desktop

The desktop build is a development harness for the same core. It lets you run
the normalizer, inspect tools and run tests without a phone:

```sh
# macOS; Linux packages are listed in docs/BUILDING.md
brew install cmake ninja pkg-config libwebsockets openssl@3 rapidjson yaml-cpp \
             sqlite jpeg-turbo libpng libtiff webp catch2

git clone https://github.com/thetahealth/mirobody-on-device.git
cd mirobody-on-device
./build.sh
build/tests/mirobody_tests
build/fhir normalize "5.62 mmol/L" "72 bpm"
build/mcp list
```

These commands inspect the core; they do not download a model or run the full
phone experience. The development profile starts the front door when you run
`build/mirobody`. It reads `config.example.yml` as a template, prefers a local
`config.yml` when present, and must never be committed with credentials.

For the closest host-side approximation to the phone profile, run:

```sh
./build.sh mobile
```

That profile omits the HTTP front door and exercises the library shape used by
the HarmonyOS native module.

## Choose a contribution path

| Goal | Start with | What you can verify first |
|---|---|---|
| Core normalization or storage | [Build guide](docs/BUILDING.md), `src/fhir/`, `tests/` | Desktop core and tests |
| Android or iOS host | [Android guide](android/README.md) or [iOS guide](ios/README.md) | Pure client UI without native sysroots; embedded core requires cross-built dependencies |
| HarmonyOS native path | [HarmonyOS guide](harmony/README.md) | NAPI path after native sysroots are prepared |
| Shared WebView UI | [mirobody-web](https://github.com/thetahealth/mirobody-web), [architecture](docs/architecture.md) | Planned integration; current app UIs remain native |

No model weights are bundled with the repository.

## Host apps

| Host | UI and native boundary | Current integration |
|---|---|---|
| Android | Kotlin / Compose + JNI | If native dependencies are present, the app can embed the loopback core; otherwise it is a remote client. Direct C ABI calls are the target. |
| iOS | SwiftUI + optional XCFramework | The checked-in project builds as a remote client by default. A locally built XCFramework enables the loopback core; direct calls are being expanded. |
| HarmonyOS | ArkUI / ArkTS + NAPI | The app embeds the mobile profile without the HTTP front door when its native dependencies are built. |

The app-specific build and signing instructions live in [android/README.md](android/README.md),
[ios/README.md](ios/README.md) and [harmony/README.md](harmony/README.md).
The cross-platform shape is documented in [docs/architecture.md](docs/architecture.md).

## C++ standard and portability

The shared core requires **C++17**. That is a deliberate platform floor:
Android's NDK and Apple's Clang/libc++ both provide a supported C++17 path, and
C++17 removes the custom `optional` compatibility layer that made the old tree
harder to read and easier to compile with mismatched flags. The repository does
not require C++20 features because the HarmonyOS cross-toolchain and the three
independent app release paths are part of the portability surface.

The platform boundary remains C, even though the implementation is C++17. The
C ABI is append-only in meaning: add a function or a versioned field instead of
changing what an existing function does.

## Repository layout

```text
src/                  shared C++17 core
  mirobody.h          public C ABI for host integrations
  platform/           C ABI implementation and platform bridges
  chat/ llm/ mcp/     agent loop, model clients and local tool registry
  fhir/ indicator/    FHIR record and terminology resolution
  health/             phone-data ingestion into FHIR Observations
  database/ storage/  SQLite and sandbox-local files
  server/             loopback front door in the development profile
res/                  agents, tools, SQLite schema and runtime data
android/ ios/ harmony/ host apps and their native bridges
tests/                C++ unit tests
cli/                  development-only inspection tools
tools/                documentation and ABI checks
docs/                 architecture, building, privacy and model guides
```

## Project status

The repository was originally a full C++ port of the old mirobody v2 server. It
was narrowed to the phone runtime in September 2026. The server databases,
object stores, Redis, cloud vendor connectors, hosted memory services, desktop
clients and Electron shell are preserved at the
[`v2-full-2026-08`](https://github.com/thetahealth/mirobody-on-device/tree/v2-full-2026-08)
tag and `archive/v2-full` branch.

The focused work is proceeding in this order:

1. **Finish the local runtime boundary.** Move Android and iOS from the
   compatibility loopback front door to the same direct C ABI shape HarmonyOS
   already exercises, then make HTTP an explicit development module.
2. **Share the contract.** Match the main repo's API, SSE event names and
   capability document; use a small host bridge for native-only actions.
3. **Share the vocabulary.** Load the versioned device bundle published by the
   main repo instead of maintaining a second device lexicon.
4. **Share the record format.** Import and export the main repo's FHIR Bundle so
   a phone backup can move to a self-hosted mirobody deployment.

These are integration milestones, not claims that every lane is finished.
Current changes are recorded in [CHANGELOG.md](CHANGELOG.md).

## Contributing

Start with [CONTRIBUTING.md](CONTRIBUTING.md). Before opening a pull request,
run the same checks as CI:

```sh
./build.sh
build/tests/mirobody_tests
./build.sh mobile
python3 tools/check_doc_links.py
python3 tools/check_exports.py
```

If a change touches Android, iOS, HarmonyOS, `src/platform/` or the C ABI and
you cannot build that host, say so in the pull request. Do not report a desktop
build as a phone build.

Useful issue reports include a phone-store field that was assigned the wrong
LOINC code or UCUM unit. Do not attach real health data, credentials or logs
containing values.

→ [CONTRIBUTING.md](CONTRIBUTING.md) · [AGENTS.md](AGENTS.md) · [SECURITY.md](SECURITY.md) · [Code of Conduct](CODE_OF_CONDUCT.md) · [CHANGELOG.md](CHANGELOG.md)

## Documentation

- [Architecture](docs/architecture.md): the core, host bridges, build profiles and data flow.
- [Building](docs/BUILDING.md): desktop builds, mobile profiles, sysroots and app toolchains.
- [Testing](docs/testing.md): CI coverage, phone app verification and PR evidence.
- [Privacy tiers](docs/privacy-tiers.md): what each lane can send off the phone.
- [On-device LLM](docs/on-device-llm.md): runtimes, formats, quantization and measurements.
- [Markdown contract](docs/markdown.md): rendering requirements for clients.
- [Colors and fonts](docs/colors-and-fonts.md): shared visual tokens.
- [Documentation index](docs/README.md): which file owns which decision.

The server API and terminology remain owned by [mirobody](https://github.com/thetahealth/mirobody)
and [docs.mirobody.ai](https://docs.mirobody.ai/). This repository follows those
contracts instead of restating them.

<div align="center">

Apache 2.0 · © 2026 [Theta Health](https://thetahealth.ai)

</div>
