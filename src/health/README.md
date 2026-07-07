# `src/health` — the health-data linking & fetch service

This module connects a mirobody user to their account on an external
health-data vendor (Terra, Validic, Vitalera, …) and pulls their wearable, lab,
and clinical records from it. Access is **ownership-gated**: a user must prove
they own the external account before any data can be read, so a self-asserted id
can never reach another person's records.

```
POST /vendors/{id}/bind ──▶ PENDING link ──verify(consent)──▶ VERIFIED link
                                                                    │
GET /vendors/{id}/data ──require VERIFIED──▶ Vendor::fetch ──▶ vendor JSON
```

Binding mirrors email binding in `src/user`: submit the external id, prove
ownership, then it becomes usable. The link lives in the `user_vendor_accounts`
table (`res/sql/<dialect>/1_health.sql`); a `UNIQUE(vendor_id, external_user_id)`
constraint is the ownership backstop — two users can't both claim one account.

Everything is `namespace mirobody::health` and mirrors the shape of
`fhir::FhirService` / `user::UserService`. The pieces:

| File | Type | Responsibility |
| --- | --- | --- |
| [`vendor_service.{hpp,cpp}`](vendor_service.hpp) | HTTP front end | wires the `/vendors/*` routes onto the `Router` |
| [`vendor_link.{hpp,cpp}`](vendor_link.hpp) | machinery | storage, config, the verified-gated fetch path (no HTTP) |
| [`vendor_fhir.{hpp,cpp}`](vendor_fhir.hpp) | mapping | vendor-native fetch JSON → FHIR R4 `Observation` bodies |
| [`ehr_connect.{hpp,cpp}`](ehr_connect.hpp) | HTTP front end | the browser-driven SMART-on-FHIR EHR connect flow |
| [`werun.{hpp,cpp}`](werun.hpp) | HTTP front end | WeChat WeRun step ingestion (decrypt → FHIR) |
| [`vendor/`](vendor/) | clients | one `vendor::Vendor` per source, resolved by id |

## `vendor_service` — `VendorService`

The route owner. Constructing it borrows the `Config`, `Database`, and `Jwt` and
wires three routes; instantiate once at startup. Every route requires a bearer JWT
(medical data), is scoped to the authenticated user, uses the project envelope
(`{"code","msg","data"}`), and takes `{id}` as a vendor key from the registry.

| Route | What it does | Backing |
| --- | --- | --- |
| `GET /vendors` | list the user's connected vendors: `[{id, verified, has_token, updated_at}]` (no secrets) | `VendorLinkStore::list` |
| `GET /vendors/icons` | icon bundle `{id: "data:…;base64,…"}` — vendor site favicons fetched once on a background thread, held in memory (public; one same-origin request, no third-party calls from the browser) | `VendorService::build_icons` |
| `GET /vendors/{id}/authorize` | build the vendor's OAuth authorize URL + stash single-use state; returns `{authorize_url}` (needs `VENDOR_REDIRECT_URI` + the vendor's `<ID>_CLIENT_ID`) | `Vendor::authorize_url` |
| `GET /vendors/callback?code=&state=` | vendor's browser redirect back (no bearer; user from `state`): exchange code → store tokens (encrypted) → mark verified; `302` to the app with `?vendor=<status>` | `oauth_connect` |
| `POST /vendors/{id}/bind` | record the external account id as a PENDING link (re-binding replaces the id + clears verification) | `upsert_pending` |
| `POST /vendors/{id}/bind/verify` | prove ownership (`verify_consent`) → mark VERIFIED. Body `{code, redirect_uri}`: the server exchanges the OAuth code and stores that user's tokens (encrypted). Empty `code` → configured-credential probe | `verify_consent` |
| `GET /vendors/{id}/data?domain=&start=&end=` | fetch data for the verified link over a time range (raw vendor JSON), using the user's stored token (auto-refreshed if expired) or the configured credential | `fetch_for_user` |
| `POST /vendors/{id}/sync?domain=&start=&end=` | fetch → map to FHIR `Observation`s → persist via `FhirStore` (no `domain` ⇒ all the vendor's domains); returns `{posted, failed}` | `fetch_for_user` + `vendor_json_to_observations` |
| `POST /vendors/{id}/unlink` | disconnect: best-effort revoke at the vendor, then delete the link row + stored tokens (GDPR/PIPL erasure). Idempotent | `unlink_for_user` |

### Per-user vendor tokens — connect, refresh, encryption

Each user connects their own vendor account over OAuth2, and the server pulls that
user's data with that user's token — refreshing it as needed. This is the whole
lifecycle.

**Connect (two entry points, same result).** The user authorizes at the vendor and
the server exchanges the returned authorization `code` (`Vendor::exchange_code`),
which both proves ownership and yields the access + refresh tokens:

- **Web** — `GET /vendors/{id}/authorize` builds the vendor's consent URL and stashes
  a single-use `state → {user, vendor}` in the cache; the browser is sent there; the
  vendor redirects back to `GET /vendors/callback`, which runs `oauth_connect`
  (exchange → store tokens → mark verified) and `302`s to the app with `?vendor=…`.
- **API** — `POST /vendors/{id}/bind/verify {code, redirect_uri}` does the same
  exchange via `verify_consent`. (With no `code`, `verify_consent` instead does a
  credential *probe* — a small read with the configured credential — for the
  self-hosted / single-user model.)

**Storage.** Tokens live in `user_vendor_accounts` — `access_token`, `refresh_token`
(both Fernet ciphertext), and `token_expires_at` (unix ms; `0`/NULL = non-expiring,
e.g. Polar). The access token is stored even when a refresh token exists (it saves a
refresh round-trip); for no-refresh, non-expiring vendors (Polar) it is the *only*
durable credential, so the column can't be dropped.

**Refresh (in `fetch_for_user`, one refresh max per call).** On every `/data` /
`/sync`:

- **Proactive** — if the stored access token has expired or is within 60 s of it
  (`token_expires_at`), refresh via `Vendor::refresh` before the fetch.
- **Reactive** — if the fetch is rejected `401/403` even though our clock said the
  token was still valid (early revocation / early expiry), refresh once and retry.
- Either way the **rotated tokens are re-persisted** (a vendor that returns a new
  refresh token replaces the old; one that doesn't keeps it). Vendors with no refresh
  grant / non-expiring tokens (Polar) skip refresh; a failed refresh falls through to
  a `401` and the user re-connects.

**Deletion.** `/vendors/{id}/unlink` best-effort revokes at the vendor then deletes
the row (`unlink_for_user` → `remove`); re-binding also clears the token columns. So
tokens don't linger after a disconnect (GDPR/PIPL erasure).

#### Encryption at rest & key rotation (`VENDOR_TOKEN_ENCRYPTION_KEY`)

Tokens are bearer secrets, so they are **only ever stored encrypted** — Fernet
(AES-128-CBC + HMAC, via `config/fernet.hpp`), keyed by `VENDOR_TOKEN_ENCRYPTION_KEY`.
It is a **list** of Fernet keys with the same contract as `FILE_ENCRYPTION_KEY`:

- **Write** — the **last** key encrypts (`token_encrypt` in `vendor_link.cpp`).
- **Read** — **every** key is tried until one decrypts (`token_decrypt`); a token
  encrypted under any listed key still decrypts. A ciphertext that no current key can
  decrypt yields `""` → treated as "no token" → the link needs re-connect.
- **Default = `FILE_ENCRYPTION_KEY`.** When `VENDOR_TOKEN_ENCRYPTION_KEY` is unset it
  falls back to `FILE_ENCRYPTION_KEY` (config.cpp) — a vendor token is user data at
  rest just like an upload, so a deployment that already encrypts uploads gets
  per-user token encryption for free. Set `VENDOR_TOKEN_ENCRYPTION_KEY` only to rotate
  the token key independently of the file key.
- **Neither key set ⇒ DB token storage is disabled**: connect still proves ownership
  but persists nothing (a plaintext token is never written), and fetch falls back to
  the configured `<ID>_*` credential.

  There is deliberately **no encryption-free token store**: process-local memory
  wouldn't survive a restart or reach a second instance, and a shared store (DB row or
  Redis) would put a plaintext bearer secret at rest (Redis persistence/replicas/
  `MONITOR` included). The only thing that is both cross-instance and encrypted-at-rest
  is the shared DB under a key — which is exactly the has-key path. So **multi-instance,
  multi-user, refresh-capable pulls require a key** (this one or `FILE_ENCRYPTION_KEY`);
  no key ⇒ effectively single-instance / the shared credential.

**To rotate the key (zero downtime):**

1. Generate a new 44-char URL-safe base64 Fernet key (same format as
   `FILE_ENCRYPTION_KEY` / `CONFIG_ENCRYPTION_KEY`).
2. **Append** it to `VENDOR_TOKEN_ENCRYPTION_KEY` as the **last** entry and restart.
   On boot, `reencrypt_vendor_tokens` migrates **every** stored token from the old key
   to the new one in a single pass (before the server serves requests, so it never
   races DB access; the token table is small). New writes already use the new key.
3. That boot leaves no token under the old key, so **drop the old key** and restart
   again — done. Back to one key ⇒ the startup pass is skipped.

The migration runs **only mid-rotation** (≥2 keys listed); a normal single-key boot
skips it entirely (zero cost). It detects "already under the newest key" by a
trial-decrypt with just that key, so it re-writes only the rows that actually moved
keys. Dropping a key before its tokens are migrated isn't catastrophic — those users
simply re-connect.

**No leakage.** Tokens are never logged or returned by the API; `Config::print` masks
the encryption key like other secrets, and `GET /vendors` exposes only a `has_token`
boolean, never the token.

## `vendor_link` — storage, config, and the fetch path

The seam between the central `Config`, the `user_vendor_accounts` table, and the
vendor clients. No HTTP here — `VendorService` calls into it.

| Symbol | Role |
| --- | --- |
| `VendorLinkStore` | CRUD over `user_vendor_accounts` (`get` / `upsert_pending` / `mark_verified`); borrows the `Database` (not owned), like `fhir::FhirStore` |
| `vendor_config(cfg, id)` | build a `vendor::VendorConfig` from the per-vendor `MIROBODY_VENDOR_<ID>_*` env convention, overlaid with central `Config` credentials (Vitalera's pre-issued bearer; Dexcom's `client_id`/`secret` + `environment`→base_url) |
| `fetch_for_user(...)` | resolve the user's **verified** external id → build the client → call `Vendor::fetch`, using the user's stored (decrypted) token, auto-refreshing + re-persisting it when expired, else the configured credential. Returns `""` + a reason when there is no link, it is unverified, the vendor is unknown, or the call throws |
| `verify_consent(...)` | **the verification seam** — with an OAuth `code`: exchange it (`Vendor::exchange_code`, proves ownership) and `save_tokens` (encrypted); without a code: fall back to a **credential probe** (read a small recent window; any successful domain read passes). Token crypto uses the `VENDOR_TOKEN_ENCRYPTION_KEY` Fernet keys |

## `vendor_fhir` — vendor JSON → FHIR `Observation`

Device-brand clients (`oura`, `whoop`, …) return each vendor's **own** JSON, not
FHIR. `vendor_json_to_observations(vendor_id, domain, json, subject_ref)` turns that
into FHIR R4 `Observation` bodies, so `/vendors/{id}/sync` can persist them through
`fhir::FhirStore` — the same write path the on-device apps and EHR connect use.

A reading is mapped only when it has a confident LOINC code + UCUM unit; metrics
without one (sleep scores, strain, HRV) are **deferred** — left unmapped and
documented — rather than mapped to a wrong code (the same honesty rule the vendor
clients follow). Currently mapped:

| Vendor | Domain | Reading | LOINC | Unit (UCUM) | Category |
| --- | --- | --- | --- | --- | --- |
| `oura` | HeartRate | `data[].bpm` | `8867-4` Heart rate | `/min` | vital-signs |
| `oura` | Activity | `data[].steps` | `55423-8` Steps (24h) | `{steps}` | activity |
| `whoop` | HeartRate | recovery `resting_heart_rate` | `40443-4` Heart rate --resting | `/min` | vital-signs |
| `whoop` | HeartRate | recovery `spo2_percentage` | `59408-5` Oxygen saturation | `%` | vital-signs |
| `dexcom` | Glucose | `records[].value` @ `systemTime` | `2339-0` Glucose in Blood | `mg/dL` / `mmol/L` (from the response `unit`) | laboratory |
| `werun` | (steps) | `stepInfoList[].step` @ `timestamp` | `55423-8` Steps (24h) | `{steps}` | activity |

Deferred (no confident code yet): Oura/WHOOP **sleep** durations, WHOOP **strain**
(cycle), HRV. Adding a metric is one row in `vendor_fhir.cpp` — the pipeline (map →
`upsert`) is already wired for every source, including WeChat WeRun (see below).

## `ehr_connect` — `EhrConnectService` (SMART on FHIR)

The browser-driven flow that connects a user's EHR (Epic, Oracle Health/Cerner, …)
and pulls their records. Here mirobody is an OAuth **client** of the EHR (distinct
from `src/oauth`, where it is the authorization *server*). It is the one health
source a **web page** can collect — Apple/Samsung/Xiaomi are on-device-only and
need the native apps. Routes are under `HTTP_URI_PREFIX`.

| Route | What it does |
| --- | --- |
| `GET /health/ehr/providers?q=&source=` | search the Service Base URL directories ([`vendor/ehr/directory.hpp`](vendor/ehr/directory.hpp)) for tenants (org name → FHIR base URL) |
| `POST /health/ehr/authorize {fhir_base_url}` | SMART discovery (`/.well-known/smart-configuration`) + PKCE; cache the verifier/state; return the `authorize_url` to redirect to |
| `GET /health/ehr/callback?code=&state=` | the EHR's redirect back (no bearer; user recovered from the single-use `state`); exchange the code, cache the token per user, `302` back to the app with `?ehr=<status>` |
| `POST /health/ehr/sync` | use the cached token to fetch Observations via the [`ehr`](vendor/ehr/ehr.cpp) client and persist each through the FHIR store |

Configured by the `SMART_FHIR_*` keys (client_id / redirect_uri / scope; see
`config.example.yml`). The web client drives it from Settings → **Connect EHR**
([htdoc/src/ehr.js](../../htdoc/src/ehr.js)).

## `werun` — `WeRunService` (WeChat steps)

WeChat's only health surface is **WeRun daily steps** — and it is not Bluetooth. A
Mini Program calls `wx.getWeRunData()`, which returns the last ~31 days of daily
steps as an AES-128-CBC-encrypted blob; this service decrypts it and persists the
steps as FHIR `Observation`s. It reuses the existing Mini Program credentials
(`WECHAT_APPID` / `WECHAT_SECRET`), so no new config.

| Route | What it does |
| --- | --- |
| `POST /wechat/werun` `{code, encryptedData, iv}` | exchange `code` via jscode2session → decrypt the blob with the session_key → map `stepInfoList` to FHIR steps (`vendor_json_to_observations("werun", …)`) → persist via `FhirStore`; returns `{posted, failed}` |

The **session_key is never stored**: the Mini Program sends a fresh `wx.login()`
`code` with the blob, exchanged use-once at decrypt time. The user is taken from the
bearer JWT; the decrypted payload's `watermark.appid` is checked against our app so a
blob captured from another app is rejected. Steps map to LOINC `55423-8` (see the
`vendor_fhir` table above).

## [`vendor/`](vendor/) — the vendor clients

One `vendor::Vendor` per source behind a common authorize / fetch / webhook
contract, resolved by id through the [registry](vendor/registry.hpp). The shared
`vendor.hpp` / `registry.*` stay at the `vendor/` root; the per-vendor clients are
sorted by source type:

| Dir | Holds | Examples |
| --- | --- | --- |
| [`device/`](vendor/device/) | consumer device brands with their own API | Dexcom, Fitbit, Garmin, Oura, Polar, WHOOP, Withings |
| [`ehr/`](vendor/ehr/) | direct EHR systems via SMART on FHIR | one generic client + [`EhrDirectory`](vendor/ehr/directory.hpp) loader |
| [`phone/`](vendor/phone/) | smartphone-vendor stores that expose a cloud API | Huawei Health Kit |
| [`platform/`](vendor/platform/) | B2B data aggregators | Terra, Validic, Thryve, Rook, Spike, Metriport, … |

Each vendor's transport is implemented against the platform's public API;
undocumented operations stay explicit stubs. See [`vendor/README.md`](vendor/README.md)
for the cross-platform comparison, the per-vendor implementation-status table, and
the individual-developer **access-model** table (free self-serve vs partner-gated).

## Device-native health platforms (Apple / Samsung / Google / Huawei / Xiaomi / …)

The phone-OS and smartphone-vendor health stores are a different shape: their data
is read **on the device** through a platform SDK, not fetched server-to-server. The
host app reads samples on-device, maps them to FHIR `Observation`s, and **POSTs them
to this server's FHIR R4 endpoint** (`src/fhir`, whose write model mirrors Android
Health Connect) — the on-device ingestion path, no route here. Implemented in the
apps: Health Connect + HMS Health Kit on Android, Apple HealthKit on iOS (see
[`android/README.md`](../../android/README.md) / [`ios/README.md`](../../ios/README.md)).

| Platform | Server-callable API? | How its data reaches mirobody |
| --- | --- | --- |
| Apple Health (HealthKit) | ❌ on-device only | iOS app reads → FHIR `Observation` → POST FHIR R4 |
| Google Health Connect | ❌ on-device only | Android app reads → FHIR R4 |
| Samsung Health | ❌ (partner cloud deprecated) | writes into Health Connect → Android app → FHIR R4 |
| Xiaomi / Mi Fitness, Honor, OPPO/HeyTap, vivo | ❌ on-device only | via Health Connect / OEM SDK on Android → FHIR R4 |
| **Huawei Health Kit** | ✅ **cloud REST** | server `fetch` via [`vendor/phone/huawei.cpp`](vendor/phone/huawei.cpp) (`sampleSet:polymerize`, Account Kit OAuth) |

On Android these OEMs increasingly converge on **Health Connect** as the single read
surface, so the app often integrates Health Connect once rather than each OEM SDK.

## Direct device-brand clients ([`vendor/device/`](vendor/device/))

Device *brands* with their own OAuth REST APIs are normally reached through the
aggregators above, so most need no dedicated client. Those below have a direct one —
useful because an individual developer who owns the device can usually self-serve
free credentials and skip the (paid) aggregators. The **individual-dev access** column
is the quick lens; [`vendor/README.md`](vendor/README.md) has the authoritative
access-model and endpoint-status tables.

| id | Brand | `fetch` domains | Individual-dev access | Notes |
| --- | --- | --- | --- | --- |
| [`dexcom`](vendor/device/dexcom.cpp) | Dexcom CGM | glucose | ✅ free to start | **cloud + retrospective** (~1h US / ~3h OUS delay, no real-time); sandbox free + ≤5-user prod, more needs a partnership; `DEXCOM_*` config |
| [`fitbit`](vendor/device/fitbit.cpp) | Fitbit | activity, heart rate, sleep, body | ✅ free self-serve | per-domain time-series GETs over a day range |
| [`garmin`](vendor/device/garmin.cpp) | Garmin | — (push) | ❌ partner-gated | OAuth1.0a, push-based; `fetch` explains the model, real path is `handle_webhook` |
| [`oura`](vendor/device/oura.cpp) | Oura Ring | sleep, activity, heart rate | ✅ free self-serve | v2 usercollection (daily by date, heart rate by datetime) |
| [`polar`](vendor/device/polar.cpp) | Polar | sleep (+ training/activity) | ✅ free self-serve | **sleep** is a direct GET; training/activity use AccessLink's transaction pull model (no `[start,end]`) so they throw with an explanation, like Garmin; `revoke` deletes the user |
| [`whoop`](vendor/device/whoop.cpp) | WHOOP | sleep, heart rate (recovery), activity (cycle) | ✅ free self-serve | needs a WHOOP device + membership; cursor-paginated |
| [`withings`](vendor/device/withings.cpp) | Withings | measures, heart, activity, sleep | ✅ free self-serve | form-encoded `action` services |

Other brands (Suunto, Ultrahuman, …) stay aggregator-only — they are partner-gated,
so add a direct client only once you hold their partner credentials.

## Direct Bluetooth devices (HDP / BLE GATT) — not yet implemented

Everything above reaches a device through a **cloud API** or an **on-device platform
store** (Health Connect / HealthKit). A third path — talking **Bluetooth directly to
the sensor** — is not built yet; this section is the map for adding it.

**Where it runs:**

- The server has no radio near the user's devices, so it **cannot** read Bluetooth.
- The read happens on a client next to the sensor — the [`qt`](../../qt/) desktop app, a native mobile layer, or a gateway.
- That client maps readings to FHIR `Observation`s and **POSTs them to the FHIR R4 endpoint** — the same on-device path Apple/Health-Connect use, so **no new server route or Bluetooth code is needed here**.
- On phones most devices need no direct connection: the OEM companion app pairs the sensor and its samples land in Health Connect / HealthKit.
- Direct BLE is only for **standard-profile medical devices with no companion app**, or desktop / kiosk / gateway scenarios.

### HDP — Health Device Profile (Classic Bluetooth)

MCAP transport + IEEE 11073-20601 exchange protocol + a `104xx` device specialization.

> ⚠️ **Legacy — do not build on HDP.** The Bluetooth SIG stopped advancing it;
> Android's `BluetoothHealth` was deprecated in API 29 then removed; iOS and Windows
> never supported it. Only relevant for existing legacy medical hardware over Linux BlueZ.

| IEEE 11073 | Device |
| --- | --- |
| `-10404` | Pulse oximeter |
| `-10406` | Basic heart rate / ECG |
| `-10407` | Blood pressure monitor |
| `-10408` | Thermometer |
| `-10415` | Weighing scale |
| `-10417` | Glucose meter |
| `-10418` | Coagulation (INR) |
| `-10420` | Body composition analyzer |
| `-10421` | Peak flow (respiratory) |
| `-10441` | Cardiovascular fitness / activity |
| `-10442` | Strength fitness |
| `-10471` | Independent living activity hub |
| `-10472` | Medication monitor |

### BLE GATT — the current standard

BLE Low Energy with standardized SIG GATT services (16-bit UUIDs); readings arrive via
Notify/Indicate or characteristic reads.

The **free direct-read examples** column lists devices that expose the *standard public*
GATT service, so an individual dev can just buy one and read it — no cloud API,
partnership, or paid tier. Blank = no reliably standard-profile consumer device (the
category is dominated by proprietary/encrypted GATT; see the caveats below).

| Service | UUID | Key characteristic | Free direct-read examples (individual dev) |
| --- | --- | --- | --- |
| Heart Rate (HRS) | `0x180D` | Heart Rate Measurement `0x2A37` | Polar H10, Polar H9, Wahoo TICKR, Garmin HRM-Dual, CooSpo H6 |
| Health Thermometer (HTS) | `0x1809` | Temperature Meas. `0x2A1C` | *(few — mostly proprietary)* |
| Blood Pressure (BLS) | `0x1810` | BP Measurement `0x2A35` | A&D UA-651BLE, A&D UA-767BLE, Beurer BM57 |
| Glucose (GLS) | `0x1808` | Glucose Meas. `0x2A18` | *(most meters proprietary)* |
| Continuous Glucose (CGMS) | `0x181F` | CGM Meas. `0x2AA7` | *(Dexcom / Libre are encrypted — none)* |
| Pulse Oximeter (PLXS) | `0x1822` | Spot `0x2A5E` / Continuous `0x2A5F` | Nonin 3230, Masimo MightySat |
| Weight Scale (WSS) | `0x181D` | Weight Meas. `0x2A9D` | A&D UC-352BLE |
| Body Composition (BCS) | `0x181B` | Body Comp. Meas. `0x2A9C` | *(mostly proprietary — Mi/Withings, etc.)* |
| Insulin Delivery (IDS) | `0x183A` | — | *(prescription pumps — not self-serve)* |
| Physical Activity Monitor (PAMS) | `0x183E` | — | |
| Fitness Machine (FTMS) | `0x1826` | — | Wahoo KICKR, Zwift Hub, Tacx Flux, Elite Suito |
| Cycling Speed & Cadence (CSC) | `0x1816` | `0x2A5B` | Wahoo RPM, Garmin Speed Sensor 2, Magene S3+ |
| Running Speed & Cadence (RSC) | `0x1814` | `0x2A53` | Stryd, Garmin RD Pod |
| Cycling Power | `0x1818` | `0x2A63` | Favero Assioma, Stages, 4iiii Precision, Garmin Rally |
| Device Information (DIS) | `0x180A` | model / serial / firmware | *(companion service — present on all above)* |
| Battery (BAS) | `0x180F` | Battery Level `0x2A19` | *(companion service — present on all above)* |

Two caveats:
- **GHS (Generic Health Sensor)** is the SIG's newer profile — IEEE 11073's data model
  carried over BLE GATT, the modern successor to HDP; worth tracking for durable
  medical-device support.
- **Consumer watches/rings (Oura, WHOOP, Fitbit, Garmin, Apple Watch, Mi/Huawei bands)
  use proprietary/encrypted GATT** and can't be read directly — they stay on the cloud
  clients in [`vendor/device/`](vendor/device/) or the platform stores. Direct BLE is
  only genuinely useful for **standard-profile medical devices**: BP cuffs, glucose
  meters, thermometers, SpO₂ meters, scales, HR straps.

### Can C++ read Bluetooth directly?

Yes — but there is **no cross-platform standard API**; each OS differs:

| Platform | Classic / HDP | BLE GATT | C++ route |
| --- | --- | --- | --- |
| Linux | BlueZ + libbluetooth (MCAP) | BlueZ D-Bus (`org.bluez`) | D-Bus C++ bindings |
| Windows | — (HDP effectively absent) | `Windows.Devices.Bluetooth` | **C++/WinRT** direct |
| macOS / iOS | ❌ none | Core Bluetooth (BLE only) | Obj-C++ bridge |
| Android | ❌ HDP API removed | `android.bluetooth.le` | NDK has no BT → **JNI** to Java |

Cross-platform C++ libraries:
- **Qt Bluetooth (QtConnectivity)** — `QLowEnergyController` for BLE GATT, `QBluetooth*`
  for classic. The [`qt`](../../qt/) desktop app already exists, so this is the natural
  fit for a desktop BLE path.
- **SimpleBLE** — lightweight, commercially-friendly cross-platform C++ BLE (Win/macOS/Linux).

**Bottom line:** don't invest in HDP; BLE GATT for standard-profile medical devices is
the only worthwhile direction; read it on the client (desktop Qt `QLowEnergyController`,
or native mobile / Health Connect / HealthKit) and feed the existing FHIR R4 endpoint —
the server stays untouched, gaining only one more FHIR write source.

### Implementation split — three codebases

Each client uses its platform's native BLE stack (matching how the apps are already
built), so a BLE reader is written **three times**, not once. All three now exist,
sharing the same decoders (HR / BP / thermometer, IEEE-11073 SFLOAT/FLOAT) and FHIR
`Observation` shape:

| Codebase | Covers | BLE stack | Language | Implementation |
| --- | --- | --- | --- | --- |
| Android app | Android | `android.bluetooth.le` | Kotlin | [`data/health/ble/`](../../android/app/src/main/java/ai/thetahealth/mirobody/data/health/ble/) |
| iOS app | iOS | Core Bluetooth | Swift | [`Data/Health/BleHealthController.swift`](../../ios/Mirobody/Data/Health/BleHealthController.swift) |
| [`qt`](../../qt/) desktop app | Windows / macOS / Linux | `QLowEnergyController` | C++ | [`qt/blehealth.cpp`](../../qt/blehealth.cpp) |

iOS and macOS both sit on Core Bluetooth but **don't share code** — iOS is the Swift
app, macOS is the Qt desktop app. The one Qt C++ implementation covers all three desktop
OSes (Qt's WinRT / Core Bluetooth / BlueZ backends). All three map to FHIR `Observation`
and POST to the same endpoint. Mobile stays on Health Connect / HealthKit first —
a direct-BLE reader there is only for standard-profile medical devices with no companion app.
