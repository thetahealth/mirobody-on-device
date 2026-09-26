# Architecture

`mirobody-on-device` is a phone runtime, not a second mirobody server. The
shared core is embedded by HarmonyOS and can be embedded by Android and iOS
when their native dependencies are supplied. The host owns platform permissions,
background scheduling, UI and secure key storage.

## Runtime shape

The three hosts do not yet share one integration path:

```text
Android  Kotlin / Compose ── HTTP ── selected backend URL
                     └── optional JNI → embedded loopback core
iOS      SwiftUI ────────── HTTP ── selected backend URL
                     └── optional XCFramework → embedded loopback core
Harmony  ArkTS ─────────── NAPI ── embedded C++17 mobile core → SQLite

Each host: health permissions + mapping → coded FHIR Observations
Each host: separate offline model path; agent tools are not wired to all of them
```

The target is a thin host calling the shared C ABI (`src/mirobody.h`) directly:
host permissions, UI and secure storage at the edge; normalization, record,
tools and agent in the embedded core. The main repo publishes terminology and
device mappings for hosts to consume as a versioned artifact. Local inference
must be connected to local tools before the full offline agent path is available.

The front door is a compatibility and development surface and is absent from
the `MIROBODY_MOBILE` profile. The checked-in config binds the standalone process to
`127.0.0.1`, but `HTTP_HOST` can explicitly override it. Embedded Android
and iOS bridges force the listener to loopback. Binding to loopback
does not authenticate another app on the same phone; per-launch
authentication is still planned.

## Responsibilities by layer

### Host app

The host app is the platform specialist:

- requests HealthKit, Health Connect or Health Service Kit permission;
- reads platform data and converts it to the core's ingestion request;
- receives files from the camera, share sheet or document picker;
- keeps provider keys and model grants in Keychain, Android Keystore or the
  HarmonyOS secure asset store;
- schedules background work through BGTaskScheduler, WorkManager or the
  HarmonyOS equivalent;
- renders chat, charts and health views;
- will own the WebView/native bridge when a shared web surface is embedded.

The host should not duplicate normalization, local tool behavior or record
semantics. Some host-local mapping tables still exist and must move to the
main repo's published device bundle.

### C ABI

`src/mirobody.h` is the public C surface. Android currently also has direct
JNI exports for its loopback compatibility path. The C ABI uses opaque handles
and C-compatible values so it can be consumed by JNI, Swift and NAPI. The
Windows export list is checked by `tools/check_exports.py`.

Rules for changing it:

1. keep existing function meanings stable;
2. add a function for a new operation;
3. make ownership and thread rules explicit in the header;
4. return structured JSON or versioned payloads when a record may gain fields;
5. update each affected host binding and the ABI check in the same change.

### Core

The core is organized around the phone data path:

1. **Collect:** host apps read permitted phone and sensor data.
2. **Map:** host adapters currently map supported fields to LOINC/UCUM and
   submit FHIR Observations. Loading a versioned mapping artifact from the
   main repo is planned.
3. **Record:** the embedded core stores FHIR resources in SQLite; file storage
   is app-managed. Model weights may be kept in user-selected locations.
4. **Tools:** the embedded agent can query local records. The separate offline
   model paths in the apps do not all use those tools yet.
5. **Model:** local and BYOK runtimes emit chat events through platform
   adapters. Cloud turns can include messages, health context and tool results.
6. **Exchange:** a portable FHIR Bundle import/export path is planned.

SQLite is the only database and the local filesystem is the only file store in
this repository. Postgres, object stores, Redis, vendor-cloud collection and
multi-user access belong in the main repo.

## Build profiles

| Profile | CMake switch | Purpose | HTTP front door |
|---|---|---|---|
| Development | `MIROBODY_MOBILE=OFF` | Desktop harness, CLIs, tests and compatibility server | Included; loopback default, explicit `HTTP_HOST` override possible |
| Mobile | `MIROBODY_MOBILE=ON` | Embedded library shape used by HarmonyOS and the future direct app path | Excluded |

Android and iOS currently retain the development-profile loopback integration
while their host bridges move to direct C ABI calls. New phone features should
be designed for the mobile profile first so they do not accidentally depend on
the server router.

## Data flow

### Local lane

```text
platform permission / BLE / picker
             │
             ▼
host adapter → field mapping → FHIR Observation → C ABI / HTTP compatibility
                                           │
                                           └── SQLite + app-managed files
                                                       │
                                               embedded agent tools

separate app offline model path ────────────────────→ local inference
```

With an embedded core and a local model configured, the supported local
ingestion and model paths need no network request. Their integration with
agent tools remains incomplete. Model downloads and terminology updates are
separate user-visible operations; they must not be hidden inside a health read.

### BYOK lane

For the embedded BYOK path, the host supplies a key for the selected provider.
The current bridge can keep it in process configuration beyond a single turn;
hosts should store its resting copy in the platform key store.
Cloud calls can include messages, health context and tool results from that
turn. Update the privacy documentation when a new artifact can cross this
boundary.

### Server lane

A configured mirobody server owns accounts, sharing, cloud providers and
multi-user policy. The app is a client of the main repo's API. The server API,
SSE event names, capability document and terminology data are upstream
contracts; this repo must not fork them for convenience.

## Evolution rules

- Prefer a focused core change over a platform-specific copy.
- Keep platform code at the edge: permissions, lifecycle, secure storage,
  rendering and OS callbacks stay in the host.
- Keep the native bridge boring. It should validate inputs, schedule work and
  translate ownership; it should not contain health semantics.
- Add a fixture that can run against both the desktop core and a host bridge
  when a C ABI operation changes.
- Use capability discovery for optional features and version pinning for
  terminology and record formats.
- Keep embedded phone listeners on loopback. The standalone development
  server currently permits an explicit `HTTP_HOST` override.

## Related projects

The structure follows patterns that have worked in other multi-platform apps:

- [Signal libsignal](https://github.com/signalapp/libsignal) keeps a shared
  implementation behind generated Java, Swift and TypeScript surfaces.
- [Anki-Android-Backend](https://github.com/ankidroid/Anki-Android-Backend) uses
  a small native bridge and generated request/response methods.
- [Bitwarden SDK](https://github.com/bitwarden/sdk-internal) publishes a pinned
  native SDK and lets clients update it through automated changes.
- [HealthGPT](https://github.com/StanfordBDHG/HealthGPT) demonstrates a health
  app that can switch between cloud, local and nearby model execution.
- [Home Assistant's external bus](https://developers.home-assistant.io/docs/frontend/external-bus/)
  shows how a WebView host can use a small versioned message bridge. Its origin
  checks are part of the security design, not an optional polish item.

These are references for boundaries and release discipline. They are not
runtime dependencies of this repository.
