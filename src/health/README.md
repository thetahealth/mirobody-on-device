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

The module is the HTTP front end (`vendor_service.*`) and the machinery behind it
(`vendor_link.*`), sitting on top of the per-vendor clients in [`vendor/`](vendor/),
plus a separate `ehr_connect.*` service for the SMART-on-FHIR EHR connect flow.
Everything is `namespace mirobody::health` and mirrors the shape of
`fhir::FhirService` / `user::UserService`.

## `vendor_service.{hpp,cpp}` — `VendorService`

The route owner. Constructing it borrows the `Config`, `Database`, and `Jwt` and
wires three routes onto the `Router`; a caller instantiates it once at startup.
Every route requires a bearer JWT (medical data) and is scoped to the
authenticated user. Responses use the project envelope (`{"code","msg","data"}`),
and `{id}` is a vendor key from the registry.

- **`POST /vendors/{id}/bind`** — record the user's external account id as a
  PENDING link (`upsert_pending`). Re-binding replaces the id and clears any
  prior verification.
- **`POST /vendors/{id}/bind/verify`** — prove ownership via the vendor's
  consent/claim flow (`verify_consent`); on success the link is marked VERIFIED.
- **`GET /vendors/{id}/data?domain=&start=&end=`** — fetch data for the verified
  link over a time range (`fetch_for_user`).

## `vendor_link.{hpp,cpp}` — storage, config, and the fetch path

The seam between the central `Config`, the `user_vendor_accounts` table, and the
vendor clients. No HTTP here — `VendorService` calls into it.

- **`VendorLinkStore`** — CRUD over `user_vendor_accounts`: `get`,
  `upsert_pending`, `mark_verified`. Borrows the `Database` (not owned), one per
  use site, like `fhir::FhirStore`.
- **`vendor_config(cfg, vendor_id)`** — build a `vendor::VendorConfig` from the
  per-vendor `MIROBODY_VENDOR_<ID>_*` env convention, overlaid with any
  credentials the central `Config` carries (today: Vitalera's `VITALERA_API_KEY`,
  used as a pre-issued bearer).
- **`fetch_for_user(...)`** — resolve the user's **verified** external id, build
  the vendor client from config, and call `Vendor::fetch`. Returns `""` with a
  reason when the user has no link, the link is unverified, the vendor is
  unknown, or the vendor call throws.
- **`verify_consent(...)`** — **the verification seam.** Proves an external id
  belongs to the caller before the link is marked verified. **Status: stub** —
  Vitalera's patient-linking lives in its gated "Monitoreds (Patients)" API, so
  this returns "not implemented" until that contract is wired in. The rest of the
  bind flow is already built around this signature, so filling it in is localized.

## `ehr_connect.{hpp,cpp}` — `EhrConnectService` (SMART on FHIR)

The browser-driven flow that connects a user's EHR (Epic, Oracle Health/Cerner, …)
and pulls their records. Here mirobody is an OAuth **client** of the EHR (distinct
from `src/oauth`, where it is the authorization *server*). It is the one health
source a **web page** can collect — Apple/Samsung/Xiaomi are on-device-only and
need the native apps. Routes (under `HTTP_URI_PREFIX`):

- **`GET /health/ehr/providers?q=&source=`** — search the Service Base URL
  directories ([`vendor/ehr/directory.hpp`](vendor/ehr/directory.hpp)) for tenants
  (org name → FHIR base URL).
- **`POST /health/ehr/authorize` `{fhir_base_url}`** — SMART discovery
  (`/.well-known/smart-configuration`) + PKCE; stashes the verifier/state in the
  cache and returns the `authorize_url` for the browser to redirect to.
- **`GET /health/ehr/callback?code=&state=`** — the EHR's redirect back (no
  bearer; the user is recovered from the single-use `state`). Exchanges the code
  at the tenant's token endpoint, caches the access token per user, and `302`s
  back to the app with `?ehr=<status>`.
- **`POST /health/ehr/sync`** — uses the cached token to fetch Observations via
  the [`ehr`](vendor/ehr/ehr.cpp) vendor client and persists each through the FHIR
  store.

Configured by the `SMART_FHIR_*` keys (client_id / redirect_uri / scope; see
`config.example.yml`). The web client drives it from Settings → **Connect EHR**
([htdoc/src/ehr.js](../../htdoc/src/ehr.js)).

## [`vendor/`](vendor/) — the vendor clients

One `vendor::Vendor` per source behind a common authorize / fetch / webhook
contract, resolved by id through the [registry](vendor/registry.hpp). The shared
`vendor.hpp` / `registry.*` stay at the `vendor/` root; the per-vendor clients are
sorted into [`vendor/platform/`](vendor/platform/) (B2B aggregators),
[`vendor/phone/`](vendor/phone/) (smartphone-vendor stores),
[`vendor/device/`](vendor/device/) (consumer device brands), and
[`vendor/ehr/`](vendor/ehr/) (direct EHR systems via SMART on FHIR — one generic
client plus an [`EhrDirectory`](vendor/ehr/directory.hpp) loader that discovers
each tenant's FHIR base URL from public Service Base URL lists). Each vendor's
transport is implemented against the platform's public API; undocumented
operations stay explicit stubs. See [`vendor/README.md`](vendor/README.md) for
the cross-platform comparison the metadata is drawn from and the per-vendor
implementation-status table.

## Device-native health platforms (Apple / Samsung / Google / Huawei / Xiaomi / …)

The phone-OS and smartphone-vendor health stores are a different shape from the
aggregators above: their data is read **on the device** through a platform SDK,
not fetched server-to-server. They split two ways:

- **On-device only — no server REST API.** **Apple Health (HealthKit)**,
  **Samsung Health**, **Google Health Connect**, **Xiaomi / Mi Fitness**, and the
  smaller Chinese OEM apps (**Honor Health**, **OPPO / HeyTap Health**,
  **vivo Health**) expose no server-callable API for third parties — Samsung's old
  partner cloud is deprecated in favor of writing into Health Connect, and Google
  Fit's REST API is shutting down. So none of these gets a `vendor::Vendor`.
  Instead the host app reads samples on-device and maps them to FHIR
  `Observation`s, which it **POSTs to this server's FHIR R4 endpoint** (`src/fhir`,
  whose write model is itself built to mirror Android Health Connect). That is the
  on-device ingestion path — no extra route here. It is implemented in the host
  apps: Health Connect + HMS Health Kit on Android and Apple HealthKit on iOS (see
  [`android/README.md`](../../android/README.md) and
  [`ios/README.md`](../../ios/README.md)). (On Android these OEMs
  increasingly converge on **Health Connect** as the single read surface, so the
  app often integrates Health Connect once rather than each OEM SDK.)
- **On-device *plus* cloud REST.** **Huawei Health Kit** is the exception: besides
  its on-device SDK it offers a Health Kit *Cloud* REST API, so it gets a real
  fetch vendor — [`vendor/phone/huawei.cpp`](vendor/phone/huawei.cpp), registered as
  `huawei`. It reads via the documented `sampleSet:polymerize` query using a
  Huawei Account Kit OAuth token; the consent flow and subscription webhooks stay
  stubs until the deployer's client credentials are wired in.

Device *brands* with their own OAuth REST APIs are normally reached through the
aggregators above (Terra, Validic, Thryve, Rook, Spike), so most need no
dedicated client. Three have direct clients for talking to them without an
aggregator:

- [`vendor/device/fitbit.cpp`](vendor/device/fitbit.cpp) (`fitbit`) — Fitbit Web API, OAuth2
  Bearer; `fetch()` issues the per-domain time-series GETs over a day range.
- [`vendor/device/withings.cpp`](vendor/device/withings.cpp) (`withings`) — Withings Health
  Mate API, OAuth2; `fetch()` POSTs the form-encoded `action` services (measures,
  heart, activity, sleep).
- [`vendor/device/garmin.cpp`](vendor/device/garmin.cpp) (`garmin`) — Garmin Health API, which
  is **partner-gated, OAuth1.0a, and push-based**: there is no synchronous pull,
  so `fetch()` explains the model and the real path is `handle_webhook()` once
  approved partner credentials exist.

Other brands (Oura, Whoop, Polar, …) stay aggregator-only — add a client only to
deliberately bypass one.
