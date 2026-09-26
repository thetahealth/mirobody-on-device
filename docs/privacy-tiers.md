# Privacy lanes: current behavior and target rules

A phone can store a health record locally, call a model on the phone, or send
work to a server. These are separate decisions. This page distinguishes the
checked-in app paths from the device-first design we are working toward. For
security reporting, see [SECURITY.md](../SECURITY.md).

## What the apps do today

| Host | Record destination | Model paths | Check before use |
|---|---|---|---|
| Android | Health sync uses the selected Backend URL. The checked-in default is `https://test.mirobody.ai`; embedding the core requires native dependencies. | Remote chat and a separate offline model path. | Backend URL and selected model. |
| iOS | Health sync uses the selected Backend URL. The default is `http://localhost:8080`, but the native framework is not bundled by default. | Remote chat and a separate offline model path. | Set a reachable backend; localhost alone does not provide a record. |
| HarmonyOS | The app embeds the mobile C++ core through NAPI and stores supported records in SQLite. | Embedded agent and a separate local model path. | Selected model lane and provider configuration. |

The apps map supported health fields to coded FHIR `Observation` resources
before submission. The offline model paths are not all connected to embedded
record tools. A local model by itself does not mean that its answers use the
local health record. Model files may reside in app storage or a user-selected
location. They are not part of the record database.

## What can cross the device boundary

| Selected path | Health record | Message and context | Credentials |
|---|---|---|---|
| Embedded core + local model | Supported records stay in the phone's SQLite database and local files when that core is the selected backend. | Local inference stays on the phone, subject to the tool integration described above. | No model provider key is required. |
| Direct BYOK provider call | Record location still follows the selected backend; BYOK does not move the whole record by itself. | The provider receives the messages, health context, attachments or tool results included in that turn. | A BYOK key is loaded into process memory and sent to that provider for authentication. |
| Configured mirobody server | Health uploads and chat requests go to the chosen server. | The server may send a turn to its configured model provider under the server's policy. | Server credentials are governed by the [main repo](https://github.com/thetahealth/mirobody). |

A backend URL controls where Android and iOS upload health data. It is not a
privacy label: a URL might point to the phone's embedded loopback process, a
home server or a hosted service. Check the actual address. A loopback listener
blocks LAN access but is still reachable by other local processes; per-launch
authentication is planned. The standalone development process allows an
explicit `HTTP_HOST` override, while the embedded Android and iOS bridges
force the listener to loopback even if configuration names a LAN address.

A provider needs plaintext input to answer. TLS protects transport; it does
not hide the turn from that provider. Storing a key in a platform secure store
protects its resting copy, but the key must be read into memory and presented
to the provider during a direct BYOK call. The hosts' secret-handling and
consent flows still need end-to-end audits; this page does not certify them.

## Target device-first contract

The desired phone experience has two explicit choices: record placement and
model destination. A fully local choice requires both an embedded record and a
local model connected to record tools. The target behavior is:

1. Show the selected backend and model destination before health data or a
   health-context turn leaves the phone. Require consent before the first
   escalation across that boundary.
2. Keep the host's permission, secure storage and lifecycle work at the edge.
   Load a versioned terminology and device mapping bundle published by the
   [main repo](https://github.com/thetahealth/mirobody); avoid divergent maps
   in each app.
3. Use direct C ABI calls for embedded operations. Retire Android and iOS
   loopback compatibility paths only after their host bindings and tests cover
   the needed operations.
4. Add per-launch authentication while a loopback path remains. Binding to
   `127.0.0.1` alone is insufficient isolation from other apps on the phone.
5. Provide a portable FHIR Bundle export/import path before claiming that a
   record can move between a phone and a server. Switching a Backend URL alone
   is not a migration or a backup.
6. Fail visibly if a provider, key or network path is unavailable. A fallback
   that sends new data off the phone requires a separate consent decision.

These are design requirements, not guarantees implemented uniformly across the
three hosts. The [architecture](architecture.md) describes the current bridges
and [build guide](BUILDING.md) states which profiles CI compiles. The main repo
owns server storage, accounts, sharing and cloud-provider policy; this repo
implements device storage with SQLite and local files.
