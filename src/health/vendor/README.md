# Market Intelligence Report

**Date:** 2026-05-25

## Global Health Data API & Aggregation Platform Competitive Analysis

A cross-sectional breakdown and technical-ecosystem positioning of 15 leading and emerging health data aggregation platforms (covering wearables, clinical EHR, and lab diagnostic data).

---

### Thesis

Health data APIs are evolving from "single-purpose wearable connectors" toward durable infrastructure that unifies three tracks at once: wearables + clinical EHR + lab diagnostics.

### Key Takeaway

The U.S. market is driven by HIPAA and demand for direct EHR write-back, where Validic and Human API hold the high ground in the ecosystem. The European market, constrained by GDPR and local-hosting requirements, sees strong performance from Thryve and Terra. Open-source self-hosting (e.g., Open Wearables, Metriport) is emerging as a new option for startups looking to lower long-term SaaS costs.

### Author

**Tanka AI Strategy Research Group**

For decision-makers in digital health, insurtech, and clinical research.

---

## Executive Summary

As wearable device adoption rises and remote patient monitoring (RPM) becomes routine in clinical medicine, interoperability between patient-generated health data (PGHD) and clinical electronic health records (EHR) has become the core bottleneck of the digital health industry.

This report provides an in-depth analysis of 15 leading health data products, covering 8 original target products (Rook, Spike, Terra, Junction, WeFitter, LexisNexis, Thryve, Validic) and 7 supplementary competitors (Human API, Vitalera, Open Wearables, Redox, Particle Health, HealthConnect CoPilot, Metriport).

### Key Findings & Trend Insights

1. **Data-source convergence:** Competition in pure wearable data aggregation (e.g., Terra, Rook) is white-hot, and leaders are rapidly incorporating lab testing (e.g., Junction) and direct clinical EHR write-back (e.g., Validic, Human API).
2. **Compliance & hosting localization:** Europe's GDPR constraints have driven the rise of locally hosted platforms such as Thryve. Under the U.S. HIPAA framework, the ability to connect to QHINs (Qualified Health Information Networks) has become the core moat for clinical data aggregators.
3. **The rise of open source:** The open-source models of Open Wearables and Metriport break the traditional SaaS monopoly on billing by monthly active users (MAU) or API call volume, offering early- and mid-stage developers a highly cost-effective self-hosted option.

---

## Market Landscape — Competitive Positioning Matrix of 15 Platforms

The 15 platforms are mapped across two core dimensions:

- **X-axis (data-source depth):** ranging from pure wearable data to deep clinical EHR and lab diagnostic data.
- **Y-axis (use-case positioning):** ranging from general consumer health/fitness/gamification scenarios to serious medical/insurance-underwriting scenarios.

> Source: Tanka AI Strategy Research Group, compiled from each platform's latest 2026 technical documentation and publicly available business data.

---

## Comprehensive Matrix — Cross-Comparison of 15 Health Data Platforms

| Product | Core Positioning | Target Customers | Data-Source Coverage | Integration Method | Compliance & Security | Key Differentiator |
| --- | --- | --- | --- | --- | --- | --- |
| **Rook Health** | Unified wearable data API | Digital health, fitness apps, insurtech | Mainstream wearables (health/activity/sleep) | REST API, cross-platform SDK, webhook | Not publicly disclosed | Modular architecture (Connect for data extraction + Score for analytics/scoring) |
| **Spike API** | Health data gateway + AI nutrition | App developers, fitness apps, labs | Activity, sleep, heart rate, nutrition, lab reports | REST API, mobile SDK, webhook | Not publicly disclosed | Built-in Nutrition AI (food-image and nutrition-label recognition) |
| **Terra API** | All-scenario unified fitness & health API | Developers, clinical research, insurance, corporate wellness | 500+ devices, 5,000+ metrics (incl. CGM, menstruation, blood) | REST API, multi-language SDK, WebSocket, webhook | HIPAA, GDPR, SOC 2 Type II | Real-time WebSocket data streams; built-in points/streak reward system |
| **Junction** | Diagnostic data integration platform | Virtual clinics, digital health, healthcare SaaS | 300+ wearables + nationwide U.S. lab testing network | Single API, multi-language SDK, white-label embed | U.S. healthcare compliance | Integrated wearables and lab testing, no test markup, supports at-home blood draws |
| **WeFitter** | Health gamification API platform | Corporate wellness, digital fitness, insurers | Data from 300+ mainstream wearable brands | REST API, mobile SDK | European compliance standards | Gamified challenge engine (team competitions) + AI biological-age scoring |
| **LexisNexis EHR** | Life-insurance underwriting EHR intelligence platform | Life insurance carriers, distributors | 30,000+ U.S. data sources; EHR, lab, BMI, behavioral data | API access with QHINs consumer-mediated consent | HIPAA authorized network | Medical Insights underwriting-attribute extraction; automatic APS-statement handoff |
| **Thryve** | European medical-grade wearable API | Insurers, digital health, clinical trials, pharma | 500+ devices, 250+ metrics (incl. cardiovascular, diabetes) | Plug-and-play API, unified data format | GDPR, HIPAA, ISO 9001/27001 | Fully developed and hosted in Europe; offers real-time trends and risk-prediction models |
| **Validic** | Enterprise PGHD infrastructure | Health systems, health plans/insurers, wellness | 700+ health devices (BP cuffs, pulse oximeters, and other PGHD) | REST API, iOS/Android SDK, direct EHR write-back | HIPAA, SOC 2 Type II | Deep integration with EHRs like Epic/Cerner; Validic Inform is now free |
| **Human API** | Consumer-controlled health data aggregator | Insurers, digital health, clinical research | EHR from 90% of U.S. hospitals + 300+ wearables | Unified RESTful API, consumer-authorization widget | HIPAA, SOC 2 | Dual-track aggregation of clinical EHR and wearable data; covers major U.S. hospitals nationwide |
| **Vitalera** | Next-generation health data API platform | Healthcare providers, RPM developers, digital health | 500+ wearable and medical devices (incl. ECG, blood glucose) | REST API, automatic code generator, FHIR connectivity | HIPAA, GDPR, ISO 27001, SaMD | Auto-generates integration code, supports the FHIR standard, certified as medical software |
| **Open Wearables** | Open-source self-hosted wearable API platform | Startups, growth-stage companies, indie developers | Apple Health, Samsung, Garmin, Polar, and more | Flutter/React Native SDK, AI-ready endpoints | Self-hosted control (compliance depends on deployment environment) | Open-source and free, no SaaS seat fees, built-in AI interface (can connect to Claude) |
| **Redox** | Healthcare interoperability platform | Healthcare IT architects, integration engineers, hospitals | Deep EHR systems (HL7, FHIR standard data) | API-driven, real-time message streams, standardized data exchange | HIPAA, SOC 2 Type II, HITRUST | Focused on high-speed, high-reliability data exchange between EHR systems and cloud applications |
| **Particle Health** | API-driven clinical data platform | Health systems, payers, value-based care teams | Longitudinal patient clinical records retrieved from nationwide healthcare networks | API, supports ADT event streams | HIPAA, SOC 2 Type II | Focused on aggregating, de-duplicating, and standardizing longitudinal patient clinical records |
| **HealthConnect** | Dual-track EHR + wearable interoperability platform | Healthcare providers, RPM developers, AI decision systems | Epic/Cerner EHR + mainstream wearables | HL7/FHIR standard API, real-time data sync | HIPAA, medical-grade security | Dual-track real-time sync purpose-built for RPM and AI clinical decision support |
| **Metriport** | Open-source medical data interoperability platform | Digital health startups, health-system developers | Major U.S. healthcare IT systems (EHR clinical data) | Modern API, developer dashboard, FHIR R4 format | HIPAA, open-source self-hosted compliance | Open-source architecture; automatically standardizes, consolidates, and de-duplicates clinical records into FHIR R4 |

---

## Market Segments

### Segment A — Consumer Health & Gamification

Represented by **Terra API** and **WeFitter**. With 5,000+ granular metrics and real-time WebSocket streams, Terra has become almost an industry standard in fitness, meditation, and early- to mid-stage health management apps. WeFitter takes a different path, combining data with a gamified challenge engine and an AI biological-age algorithm to focus on corporate wellbeing and engagement of insurance customers.

### Segment B — Serious Medicine & Remote Patient Monitoring (RPM)

Represented by **Validic**, **Vitalera**, and **Junction**. Validic has deep roots in health systems, and its core moat lies in writing PGHD generated in patients' homes (e.g., blood pressure, blood glucose) directly into mainstream EHR systems such as Epic and Cerner. Junction innovatively connects wearable data with a nationwide U.S. lab testing network (at-home blood draws / brick-and-mortar locations), providing virtual clinics with a one-stop diagnostic-data loop.

### Segment C — Insurance Underwriting & Clinical Interoperability

Represented by **LexisNexis EHR**, **Human API**, and **Redox**. LexisNexis focuses on life-insurance underwriting, extracting Medical Insights from 30,000 data sources and shortening the underwriting cycle by 9 days. Human API and Redox act as data highways, connecting the clinical records of the vast majority of U.S. hospitals to provide compliant, high-trust data support for insurance underwriting and clinical-trial recruitment.

---

## Technical Architecture — Integration & Data-Flow Comparison

### Evolution of Mainstream Integration Patterns

1. **Traditional asynchronous webhook sync** — e.g., Rook, Spike. Device data is generated → vendor cloud → aggregator platform → webhook push to the developer's server. Latency is typically on the order of minutes, suitable for routine health monitoring.
2. **Real-time WebSocket data streams** — e.g., Terra API. Supports high-frequency, low-latency real-time data transfer, suitable for serious scenarios such as real-time heart-rate monitoring during exercise and real-time vital-sign alerts for high-risk patients.
3. **FHIR R4 / HL7 clinical-standard integration** — e.g., Redox, Metriport, Vitalera. Normalizes heterogeneous wearable or clinical data into the international FHIR standard format, enabling seamless interoperability with hospital EHR systems.

### Comparison of Data-Source Connectivity Breadth

A comparison of the number of independent wearable device brands or clinical data sources (hospitals/labs) each platform officially claims to support.

---

## Regulatory & Compliance — Industry Compliance & Barriers to Entry

### The Geographic Compliance Divide

Health data is extremely sensitive personal-privacy data, and every major jurisdiction worldwide imposes very high barriers to entry:

- **United States (HIPAA + SOC 2):** Emphasizes the patient's right to informed consent (consumer-mediated consent). Validic, Human API, and Junction all deeply embed HIPAA-compliant authorization widgets that let users complete authorization with a single sign-in to their hospital patient portal or device account.
- **Europe (GDPR + local hosting):** Europe places extremely strict limits on cross-border data transfers. Thryve's core selling point is precisely that it is "fully developed and hosted within Europe," ensuring that the health data of all European citizens never flows to overseas servers — giving it an overwhelming compliance advantage in the European insurance and clinical-trial markets.

### Compliance Checklist

Compliance credentials that enterprises must audit when selecting a health data API platform:

| Credential | Description |
| --- | --- |
| **HIPAA Compliant** | U.S. Health Insurance Portability and Accountability Act |
| **GDPR Compliant** | EU General Data Protection Regulation |
| **SOC 2 Type II** | Continuous security and privacy audit certification |
| **ISO 27001** | Information security management system certification |

> *Note: Early- and mid-stage startups that choose an open-source self-hosted solution (e.g., Open Wearables) must bear responsibility for the HIPAA/GDPR compliance audit of their own server deployment environment.*

---

## Strategic Implications — Decision Guidance & Selection Guide

### Option A: Maximizing Cost-Effectiveness & Data Autonomy

**Best for:** early- and mid-stage digital health startup teams, indie developers, research groups.

Consider an open-source self-hosted solution such as **Open Wearables** or **Metriport**. This avoids expensive monthly subscription fees (e.g., Terra starts at $399/month), and data is fully retained on your own servers, making it convenient for in-depth AI model training down the road.

### Option B: Serious Medicine, RPM & Clinical Trials

**Best for:** hospital systems, pharmaceutical companies, virtual clinics, serious medical SaaS.

The top choices are **Validic** (deep direct PGHD write-back to EHR) or **Vitalera** (SaMD medical-software certified, supports the FHIR standard). If your business involves clinical diagnostics, you can integrate **Junction** to connect to the nationwide U.S. lab testing network.

### Option C: Insurtech & Health-Insurance Innovation

**Best for:** life insurers, health-insurance underwriting departments, corporate wellness platforms.

In the U.S. market, the top choices are **LexisNexis EHR** or **Human API**, leveraging their vast clinical history records to accelerate underwriting. In the European market, **Thryve** is the top choice for meeting the extremely stringent GDPR local-hosting requirements. If the focus is on post-policy customer health promotion, you can integrate **WeFitter**'s gamified challenge engine.

---

## Implementation Status — mirobody Vendor Clients

> **Engineering addendum (2026-06-05).** The sections above are the market report. This
> section tracks the state of the actual clients under `src/health/vendor/` (sorted into
> `platform/`, `phone/`, `device/`). The matrix above is the 15 aggregation platforms; the
> registry also carries device-native and device-brand clients listed at the end of this section.
> The metadata for
> all 15 vendors (the matrix above, queryable via `Vendor::info()`) has always been real; what
> follows is the transport status. Each client was implemented against the vendor's *public*
> API reference under three rules: (1) implement only operations with a confirmed public
> contract; (2) where the field-level reference is gated, centralize the inferred names as
> constants with a confirmed-vs-inferred comment block; (3) never invent a base URL or endpoint
> — vendors with no public self-serve API get a standards-based (FHIR/HL7) or deployer-driven
> path, and undocumented operations remain `VendorError("… not implemented")` stubs.

**Config** is per the `from_env()` convention: `MIROBODY_VENDOR_<ID>_API_KEY`,
`_CLIENT_ID`, `_CLIENT_SECRET`, `_BASE_URL`. "base_url **required**" means the host is
per-deployment (self-hosted or contract-gated), so the client refuses rather than guess one.

| ID | base_url | Auth | Implemented | Stubbed (undocumented) |
| --- | --- | --- | --- | --- |
| **rook** | default `api.rook-connect.com` (sandbox `…review`) | Basic (uuid:secret) | `fetch`, `authorize_url`, `list_providers` (user-scoped), `revoke` (per data source) | `handle_webhook` (gated HMAC) |
| **spike** | default `app-api.spikeapi.com/v3` | HMAC → JWT bearer | `fetch`, Nutrition-AI image/label, `handle_webhook`, `authorize_url` (per provider), `revoke` (per provider) | `list_providers` (no catalogue endpoint) |
| **terra** | default `api.tryterra.co/v2` | `dev-id` + `x-api-key` | `authorize_url` (Connect widget), `fetch`, `handle_webhook` (HMAC `terra-signature`), `list_providers`, `revoke` | — |
| **junction** | default `api.us.junction.com` (EU/sandbox via override) | `X-Vital-API-Key` | `fetch` (wearables + labs), `list_providers`, `revoke`, `authorize_url` (Link Token), `handle_webhook` (Svix), link helpers | — |
| **wefitter** | default `api.wefitter.com/api/v1.1` | Basic → JWT bearer | `fetch`, `revoke`, `authorize_url` (per-profile connections), `list_providers` (per-profile) | `handle_webhook` (no public signature scheme) |
| **lexisnexis** | **required** | bearer | deployer-driven `fetch` | all others — no public API (sales-gated) |
| **thryve** | default `api.thryve.de` (EU) | dual Basic headers | `fetch`, `authorize_url`, `revoke` | `list_providers`, `handle_webhook` |
| **validic** | default `api.v2.validic.com` | org-id path + token query | `fetch`, `provision_user` | EHR write-back, `authorize_url`, `list_providers`, `handle_webhook`, `revoke` |
| **human_api** | default `api.humanapi.co/v1/human` | bearer | `fetch` | `authorize_url` (client-side JS), `list_providers`, `handle_webhook`, `revoke` |
| **vitalera** | default `api.vitalera.io/api` | JWT bearer | `fetch` (native + FHIR R5 Clinical), `revoke`, `list_providers`, `handle_webhook` (HMAC `x-webhook-signature`) | `authorize_url` (params gated) |
| **open_wearables** | **required** (self-hosted) | `X-Open-Wearables-API-Key` | `fetch`, `list_providers`, `authorize_url` (per provider) | `revoke`, `handle_webhook` (undocumented) |
| **redox** | **required** (embeds slug/env) | bearer (out-of-band signed JWT) | `fetch` (FHIR R4) | streaming/message API, others |
| **particle_health** | default `api.particlehealth.com` | client-creds → JWT | `fetch` (FHIR R4 `$everything`) | `handle_webhook` (ADT stream), `authorize_url`, `list_providers`, `revoke` |
| **healthconnect** | **required** | OAuth2 bearer | `fetch` (FHIR R4 `Observation`) | real-time sync, consent, others — no public API |
| **metriport** | default `api.metriport.com` (sandbox/self-host via override) | `x-api-key` | `fetch` (Medical consolidated FHIR + Devices), `authorize_url` (Connect widget), `handle_webhook` (HMAC `x-metriport-signature`), `revoke` | `list_providers` (no catalogue endpoint) |

**Notes.** `vitalera`, `healthconnect`, `redox`, `particle_health` and `metriport` (Medical) return
FHIR resources; the rest return each vendor's native JSON. Several vendors' consent flows are
user-scoped or client-side (e.g. Human API's Connect popup, Junction/WeFitter per-profile link
tokens) and so do not fit the `authorize_url(redirect_uri, state)` signature — those are
documented in-file and left as stubs rather than forced. Inferred names (a handful of date/metric
param names, auth-body fields, HMAC encodings) are flagged per-file; reconcile them against the
vendor's credentialed reference when onboarding.

### Device-native & device-brand clients (beyond the 15-platform matrix)

These are not data-aggregation platforms but smartphone-vendor health stores
(`phone/`) and consumer device brands (`device/`), added to the registry after the
report. The **on-device-only** stores — Apple Health, Samsung Health, Google Health
Connect, Xiaomi / Mi Fitness, and the Chinese OEM apps (Honor, OPPO/HeyTap, vivo) —
have **no client**: they expose no server API, so their data reaches mirobody via
the FHIR R4 write endpoint (see [../README.md](../README.md)). Those with a server
API get a client:

> **WeChat WeRun** is another non-`vendor::Vendor` source in the same spirit: its
> only data is daily steps, obtained client-side (`wx.getWeRunData()`) and pushed to
> the server as an encrypted blob — there is no server-side pull to fit the `Vendor`
> `fetch()` contract, so it has no client here. It lives as its own service at
> [`../werun.cpp`](../werun.cpp) (route `POST /wechat/werun`), decrypting the blob and
> persisting steps through the FHIR write path like the on-device stores above.

| ID | Dir | base_url | Auth | Implemented | Stubbed / notes |
| --- | --- | --- | --- | --- | --- |
| **huawei** | `phone/` | default `health-api.cloud.huawei.com` | OAuth2 bearer (Account Kit) | `fetch` (`sampleSet:polymerize`) | consent OAuth, subscription webhooks |
| **fitbit** | `device/` | default `api.fitbit.com` | OAuth2 bearer | `fetch` (per-domain time-series GETs), `authorize_url` (OAuth2 code URL), `revoke` (`/oauth2/revoke`, Basic), `handle_webhook` (HMAC-SHA1 `X-Fitbit-Signature`) | — |
| **withings** | `device/` | default `wbsapi.withings.net` | OAuth2 bearer | `fetch` (form `action` services), `authorize_url` (OAuth2 code URL) | `handle_webhook` (no inbound signature), `revoke` (no token-revoke endpoint) |
| **garmin** | `device/` | `apis.garmin.com` (push) | OAuth2.0 + PKCE (partner-gated) | `revoke` (deregister hook) | `fetch` (push model + partner-gated spec), `authorize_url` (PKCE needs a stateful verifier — see ehr_connect), `handle_webhook` (unsigned push) |
| **dexcom** | `device/` | default sandbox `sandbox-api.dexcom.com` (prod `api.dexcom.com` / `api.dexcom.eu` via `DEXCOM_ENVIRONMENT`) | OAuth2 bearer | `fetch` (v3 `egvs`, glucose — retrospective, ~1h US / ~3h OUS delay), `authorize_url` (v2 `/oauth2/login`) | `revoke` (no token-revoke endpoint), `list_providers` (single brand), `handle_webhook` (no signed webhook on the partner API) |
| **oura** | `device/` | default `api.ouraring.com` | OAuth2 bearer | `fetch` (v2 usercollection: `daily_activity`/`daily_sleep` by date, `heartrate` by datetime), `authorize_url` (`cloud.ouraring.com/oauth/authorize`) | `revoke`, `list_providers`, `handle_webhook` (webhook API is a separate contract) |
| **whoop** | `device/` | default `api.prod.whoop.com` | OAuth2 bearer | `fetch` (v2 `activity/sleep`, `recovery` [HR/HRV], `cycle` [strain]; cursor-paginated), `authorize_url` (`/oauth/oauth2/auth`) | `revoke`, `list_providers`, `handle_webhook` |
| **polar** | `device/` | default `www.polaraccesslink.com` | OAuth2 bearer | `fetch` (**sleep only** — direct GET), `authorize_url` (`flow.polar.com`), `revoke` (`DELETE /v3/users/{id}`) | `fetch` for training/activity (transaction pull model: create/list/get/commit — no `[start,end]` query, doesn't fit `fetch()`), `list_providers`, `handle_webhook` (ping) |

#### Access model — who can use each brand for FREE

This project is open source, and an individual developer who owns a device usually
will NOT pay for an aggregator (Terra/Validic/Rook) — but most brands run a **free,
self-serve developer program** you can register for with just the device (or even
just an account) and use at small scale. This table is that lens: what it takes for
one developer to get credentials. Device-brand credentials are configured through
the config object with clean per-vendor keys — `<ID>_CLIENT_ID` / `<ID>_CLIENT_SECRET`
/ `<ID>_API_KEY` / `<ID>_BASE_URL` (e.g. `OURA_CLIENT_ID`), settable in the YAML file
or as the same-named env var; see `config.example.yml`. The B2B aggregators (Terra,
Validic, Rook, …) use the same `<ID>_*` keys. (Only the standalone `vendor` CLI still
reads the older `MIROBODY_VENDOR_<ID>_*` env convention on its own; `ehr` is driven by
the EHR connect flow, not static keys.)

Beyond the static credential, the OAuth device brands implement the token lifecycle —
`exchange_code()` (authorization code → tokens) and `refresh()` — via the shared
[`oauth2.hpp`](oauth2.hpp) helper (Withings uses its own `{status,body}` envelope;
Polar's tokens don't expire, so it has no `refresh`). `/bind/verify` exchanges the
code and stores the user's tokens encrypted; `fetch` refreshes them on expiry. See
"Per-user vendor tokens" in [../README.md](../README.md).

| Brand | Individual-developer access | Notes |
| --- | --- | --- |
| **fitbit** | ✅ Free, self-serve | Register an app at dev.fitbit.com; OAuth2, no approval to start. |
| **withings** | ✅ Free, self-serve | Register at the Withings developer dashboard; OAuth2. |
| **oura** | ✅ Free, self-serve | Register an app in the Oura developer portal; OAuth2 (Personal Access Tokens were deprecated Dec 2025). Up to 10 users before Oura approval; unlimited after. |
| **whoop** | ✅ Free, self-serve | Self-serve Developer Dashboard; OAuth2. You must own a WHOOP (device + membership) to use the platform at all — which the target developer does. Up to 5 apps. |
| **polar** | ✅ Free, self-serve | Any free Polar Flow account can create an AccessLink client at admin.polaraccesslink.com; OAuth2, no approval period. |
| **dexcom** | ✅ Free to start | Free sandbox (immediate) + production Limited Access (≤5 real users) with only app registration; serving >5 real users needs a Dexcom commercial partnership + Data Licensing Agreement. Glucose is PHI → BAA for production. |
| **garmin** | ❌ Partner-gated | Garmin Health API is not self-serve — it requires approved partner credentials (OAuth1.0a/PKCE, push-based). An individual developer generally cannot self-onboard. |
| **huawei** | ⚠️ Developer account + scopes | HMS Health Kit needs a Huawei developer account and registered Health Kit scopes (region-dependent); heavier than the self-serve brands above. |
| _Strava_ | ❌ Not free (excluded) | Deliberately **not** added: Standard-Tier API access requires a paid Strava subscription (~$11.99/mo) after a 3-month grace period, so it fails the "free for individual developers" bar. If added later it belongs in a new `app/` dir (activity/social platform, not a device brand), not `device/`. |
| _Ultrahuman, Suunto_ | ❌ Partner-gated (not added) | Ultrahuman's Partnership API needs a key + Partner ID by request; Suunto explicitly does not offer API access for personal use. Neither is self-serve. |

### Direct EHR systems — SMART on FHIR (`ehr/`)

Distinct from the EHR-network aggregators above (Redox, Particle Health, Metriport
sit on TEFCA/Carequality). The US ONC Cures Act forces every certified EHR onto
the **SMART App Launch + FHIR R4** standard, so a *single* generic client covers
all of them — Epic, Oracle Health/Cerner, athenahealth, MEDITECH, Veradigm, … —
parameterized by the tenant's `base_url`.

| ID | Dir | base_url | Auth | Implemented | Stubbed / notes |
| --- | --- | --- | --- | --- | --- |
| **ehr** | `ehr/` | **required** (per-tenant FHIR base) | SMART-on-FHIR OAuth2 bearer | `fetch` (FHIR R4 `Observation` search by category + date) | `authorize_url` (per-tenant SMART discovery), others |

Tenant base URLs come from the public **Service Base URL** directories every
developer must publish (FHIR `Bundle`s of `Endpoint`s). [`ehr/directory.hpp`](directory.hpp)
loads them behind one `EhrDirectory` interface, with two sources today — **ONC
Lantern** (the national aggregator) and **Oracle Health/Cerner** (its published
bundle) — and `fetch_all()` to merge them. Both verified live 2026-06; the exact
export paths are configurable since they move.

---

## Methodology & Sources

**Methodology:** This report combines desk research with technical auditing. Technical parameters and business boundaries were extracted by systematically comparing each of the 15 platforms' official developer docs, API references, compliance whitepapers, and public business news.

**Limitations:** The pricing models of some non-open-source platforms (e.g., Spike, Redox) are non-public trade secrets, so the report relies on "contact sales for a quote" or estimates based on industry norms. The number of connected sources each platform claims (e.g., LexisNexis's 30,000+ data sources) reflects officially disclosed figures; actual availability is limited by geography and by how open specific healthcare-provider interfaces are.

### Sources Cited

- Rook Health Developer Documentation — <https://docs.tryrook.io/docs/>
- Spike API Overview & Integration Guide — <https://docs.spikeapi.com/overview>
- Terra API Reference & Pricing — <https://tryterra.co>
- Junction Diagnostic Integration Portal — <https://www.junction.com>
- WeFitter Gamification API Docs — <https://www.wefitter.com/en-us/>
- LexisNexis Health Intelligence EHR Product Sheet — risk.lexisnexis.com
- Thryve Health Wearable API Specification — <https://www.thryve.health>
- Validic Inform & RPM Platform Documentation — <https://validic.com>
- Human API Developer Portal — <https://www.humanapi.co>
- Vitalera API & SaMD Compliance Whitepaper — <https://www.vitalera.io>
- Open Wearables Open-Source Repository — <https://www.themomentum.ai>
- Redox Engine Interoperability Docs — <https://www.redoxengine.com>
- Particle Health Clinical API Reference — <https://www.particlehealth.com>
- Metriport Open-Source FHIR R4 Engine — <https://www.metriport.com>

---

*© 2026 Tanka AI. All rights reserved. Confidential Document for Internal Strategic Decision Making.*
