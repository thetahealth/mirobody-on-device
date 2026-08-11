# Mirobody Android

Android client for [mirobody](https://github.com/thetahealth/mirobody). The primary surface is **chat with Agents** (DeepAgent / MixAgent / BaselineAgent), streamed from the backend's `/api/chat` over Server-Sent Events.

- **Package:** `ai.thetahealth.mirobody`
- **minSdk:** 26 / **targetSdk:** 35 / **JDK:** source+target 17; the build itself runs on any JDK 17+ (Android Studio's bundled JBR is fine — there is no pinned daemon JVM)
- **UI:** Jetpack Compose + Material 3 + Navigation
- **Networking:** OkHttp (with SSE) + Retrofit + kotlinx.serialization
- **Storage:** DataStore (Preferences)
- **Auth:** email-code (primary) + Firebase Auth for Google sign-in
- **Markdown:** Markwon (core / tables / strikethrough / html / linkify / latex), plus our own `$…$` math, SVG figures and link-scheme allowlist — see *Rendering a reply*

## Deployment

Dual-mode and **no built-in cloud URL** — the user configures the BASE_URL inside the app (Settings → Server URL). The same build can point at a self-hosted mirobody instance or at a team-provided cloud endpoint.

The Firebase project, however, **is** baked into the build via `app/src/main/assets/google-services.json` (loaded at process start by `FirebaseInitializer`). A self-hosted deployment that wants its own Google OAuth client must replace that file and rebuild.

## Authentication

Two paths, both producing the same backend JWT (Bearer, HS256, 30-day TTL — no refresh endpoint; on expiry the client repeats the flow).

**Email + verification code (primary):**

1. `POST /email/login` triggers the verification email.
2. `POST /email/verify` exchanges the 6-digit code for an `access_token`.

The form is the staircase `htdoc/src/login.js` uses, each step unlocking the next (`ui/auth/AuthViewModels.kt` — `EmailUiState`): a valid-looking address (`*@*.*`, stricter than the server's `normalize_email`) unlocks **Send code**; a successful send unlocks the code field and starts a 60s resend cooldown (editing the address re-locks the field until a code goes to that one); six digits unlock **Sign in** and submit on their own. A rejected code stays in the field, tinted, so a mistyped digit is one backspace away. One status line under the form reports every step — the server's message when it sent one, else a localized fallback.

**Google sign-in (Firebase):**

1. `FirebaseAuth.startActivityForSignInWithProvider("google.com", activity)` runs OAuth in a Custom Tab against `accounts.google.com` — no Google Play Services required at runtime.
2. The resulting Firebase ID token is forwarded to `POST /firebase/verify`, which mints the backend JWT (the generic Firebase verifier — every Firebase-brokered provider lands here; the backend reads the email from the token's claims).

**Apple sign-in (Firebase):**

1. Same Firebase OAuthProvider mechanism as Google, with provider id `apple.com` (`AppleAuthRepository`). Android has no native Apple API, so the Firebase web flow is used; the resulting Firebase ID token goes to `POST /firebase/verify` (the Firebase validator reads the email regardless of which provider issued it).
2. **Setup:** enable Apple under Firebase console → Authentication → Sign-in method → Apple (configure the Services ID + key there). No on-device Apple credentials are needed.

**WeChat sign-in (OpenSDK):**

1. `WechatAuthRepository` registers `BuildConfig.WECHAT_APP_ID` with the WeChat OpenSDK and sends a `SendAuth.Req` (scope `snsapi_userinfo`); WeChat returns the OAuth code to `wxapi.WXEntryActivity`, which is exchanged via `POST /wechat/verify` with `flow=app`.
2. **Setup:**
   - Register a **Mobile Application** on the [WeChat Open Platform](https://open.weixin.qq.com) with this app's package name (`ai.thetahealth.mirobody`) and signing signature; note its AppID/AppSecret.
   - Build with the AppID: `./gradlew assembleDebug -PwechatAppId=wxXXXXXXXXXXXXXXXX` (or set `wechatAppId` in `gradle.properties`). Blank ⇒ the WeChat button still shows but reports "not configured" on tap.
   - Set `WECHAT_OPEN_APPID` / `WECHAT_OPEN_SECRET` on the backend to the **same** Mobile Application credentials (they fall back to `WECHAT_WEB_*` then `WECHAT_APPID`).
   - Requires the WeChat app installed on the device.

## Implemented screens

- Email input + code verification, and Google sign-in (`ui/auth`)
- Agent chat (`ui/chat/ChatScreen`):
  - Incremental `thinking` / `reply` rendering with Markdown (Markwon) — see *Rendering a reply*.
  - Slash commands typed in the composer: `/help` (the built-in guide), `/new`, `/incognito`.
  - Expandable tool-call cards driven by `queryTitle` / `queryArguments` / `queryDetail`.
  - Inline images from `image` events, tap to open the fullscreen viewer with save-to-gallery (`ui/chat/ImageViewerDialog`).
  - Per-turn cost statistics dialog from `costStatistics`.
  - **On-device option**: run a model locally and offline — pick a downloaded model (**Gemma 4**, **Qwen3**, …) shown as **"&lt;name&gt; · On-device"**, or open the on-device AI manager to download one (see *On-device LLM* below).
- Session history with per-row delete, lazily loaded — `/api/history` is not called until the navigation drawer is opened (`ui/chat/HistoryScreen`). Each row carries a "*2 hr ago · 12 messages*" subtitle: relative for the past week then an absolute date (HarmonyOS's ladder, ported — a bare clock time on a three-week-old session reads as "today"). The count needs a backend that sends `message_count`; without it the row shows the time alone rather than "0 messages".
- Server URL configuration with presets (`ui/settings/BaseUrlScreen`)
- Language and font-size preferences (`ui/settings`)
- On-device health sync (Settings → **Sync health data**, `ui/health/HealthSyncDialog`)

## Rendering a reply

The syntax every mirobody client targets — and where this one stands against it — is
[`docs/markdown.md`](../docs/markdown.md); that file is the target, this section is how it
is met here. Markwon covers GFM, and four things it does not are ours:

- **`$…$` inline math** (`ui/chat/InlineMath.kt`). Markwon's own inline processor requires
  *two* dollar signs, so single-dollar math used to reach the screen with its dollars
  intact — and worse, its `match()` searches forward from the current index rather than
  anchoring at it, so a `$` in prose could pair with a `$$…$$` further along and swallow
  the text in between. Ours is a **total** handler for `$`: it always returns a node, so
  the buggy one is never reached. Whether a `$…$` pair is math or a price is decided by
  `looksLikeTex`, ported from HarmonyOS so the two hand-written clients agree.
- **` ```svg ` figures** (`ui/chat/SvgFigure.kt`). Coil's `SvgDecoder` is already
  registered app-wide, so the renderer is free; what is not free is refusing untrusted
  markup (validate and refuse, never rewrite) and taking the aspect ratio from `viewBox`.
  A source that fails to decode demotes to its own XML.
- **Link-scheme allowlist** (`ui/chat/MarkdownText.kt`). Markwon's default fires
  `ACTION_VIEW` on whatever a link says — and coerces a scheme-less link to `https`. Only
  `http`, `https` and `mailto` are handed to the system: the link text is model output,
  and an implicit intent chosen by generated text is not something to launch.
- **Segment splitting** (`ui/chat/MarkdownSegments.kt`). A message carrying an SVG or
  chart fence is broken into text / figure / chart before layout, so a figure sits between
  the paragraph that introduces it and the one that reads it. An **unclosed** fence stays
  text: half an SVG is not a figure, and a reply is re-split on every streamed token.

`ui/chat/SlashCommand.kt` answers `/help`, `/new` and `/incognito` in the composer without
a turn. `/help` renders `assets/help/help-{en,zh}.md`, which is written as a model reply on
purpose: it exercises every construct above, so a rendering regression shows up on a device
with no model, no network and no health permission. Its message is flagged `local` and is
therefore never replayed to the on-device model and never written to history.

`MarkdownRenderingTest` covers the decisions (math vs currency, fence splitting, SVG
refusal and sizing, command matching) as plain JVM tests: `build-app.cmd
:app:testPhoneDebugUnitTest` (`build-app.sh` on macOS/Linux).

## UI conventions

**Dialogs: a ✕ in the title, and a bottom button only when there is a choice to make.**
Material3's `AlertDialog` has no slot for a close affordance, which is how the app drifted
into two kinds — the four full-screen `Dialog`s (image viewer, care circle, EHR, vendors)
each grew their own ✕, and all fourteen `AlertDialog`s had none. `ui/DialogTitle.kt`'s
`DialogTitleWithClose` goes in the `title` slot and settles it. Two rules for a new dialog:

- Its ✕ takes the **same lambda its cancel button does**, not a bare `onDismiss`. Several
  dialogs unwind work on the way out (`{ vm.disconnect(); onDismiss() }` for BLE,
  `{ vm.stop(); onDismiss() }` for HDP); two exits that leave different state is a bug.
- If leaving is the **only** thing the dialog does, it gets no bottom button — the ✕ is
  the way out, and a "Close" beside it is the same action twice. That is why the stats and
  on-device-model dialogs pass `confirmButton = {}`. A dialog with a real action keeps its
  cancel, so the pair reads as a choice.

**The composer caption** ("Mirobody is AI and can make mistakes") is fitted to exactly one
line: `AiDisclaimer` measures the string against the width actually available and steps the
size down to a 6sp floor, scaling line height and tracking with it. One line is the
requirement, not legibility — a second line steals height from the conversation on every
screen. Compose 1.7 has no `autoSize` (it landed in foundation 1.8), hence the measure loop.

**System bars.** `enableEdgeToEdge()` is called with **both** bars explicitly transparent
*and* `isNavigationBarContrastEnforced = false` on API 29+. Both are needed and they are
different mechanisms: on API 26–28 the default `SystemBarStyle.auto(...)` writes a
90%-opaque **white** scrim into `navigationBarColor`; on API 29+ that scrim is already
transparent, but the same `auto` sets contrast-enforcement true and the *platform* then
draws its own. No `SystemBarStyle` argument can turn that off — `auto()` always sets it.
Nothing replaces either: `android:windowBackground` is already the exact page colour in both
`values/` and `values-night/themes.xml`.

## On-device LLM (private chat)

Alongside the server's agents, the provider picker exposes an **on-device AI manager**
plus any **downloaded on-device models** — **Qwen3 1.7B**, **Gemma 4 E2B**,
**Qwen3 4B (Instruct)**, **Gemma 4 E4B**, shown
as **"&lt;name&gt; · On-device"** — chat that runs entirely on the phone (no network,
no server), emitting the same `reply` stream so the chat UI is unchanged (`data/llm/`).
It stays available even when the server is unreachable.

- **Two engines, and a model says which it needs** (`OnDeviceRuntime` on its spec):
  - **LiteRT-LM** (`com.google.ai.edge.litertlm`, a Gradle dependency) runs `.litertlm`
    files from the `litert-community` HF org — and is the faster of the two on **Gemma's
    E-series**, whose MatFormer / per-layer-embedding design Google's own runtime
    exploits. It also has the only GPU builds.
  - **llama.cpp** runs any **GGUF**, cross-compiled into `libmirobody.so` by
    `build-llama.cmd` (see *On-device llama.cpp* below). Faster on anything plainly
    dense — 1.6× LiteRT's decode on Qwen3 4B — and the only way to run a model nobody
    has converted to `.litertlm`.

  Neither wins outright — *Measured* below has the numbers. Both stream, and
  `OnDeviceEngines` routes each model to the one its spec names, so nothing above that
  seam knows there are two. `LlamaCppEngine` is lifecycle only: the turn itself is the
  same `llm::LocalClient` HarmonyOS and Qt use, reached through `NativeBridge`, so the
  context ladder, sampling, the `<think>` split and the sliding history window are fixed
  once for three frontends.

  The catalog is split down the middle — **Gemma on LiteRT-LM, Qwen on llama.cpp** — and
  that is availability, not preference. The GGUF lane takes any architecture llama.cpp
  already supports; the `.litertlm` lane takes only what someone has converted, and
  `litert-community` publishes no Qwen3.5 above 0.8B (its GatedDeltaNet layers need a
  hybrid-cache patch on top of `litert-torch` before they convert at all). The Qwen**3**
  `.litertlm` builds the catalog used to offer stay in `DEBUG_MODELS`, because "which
  engine is faster" is only answerable while both files are still downloadable.
- **Models** are **not bundled** — `ModelManager`
  downloads each on demand from Hugging Face (resumable, with retry-and-resume, and
  forced onto HTTP/1.1 — a 3 GB body held open for minutes is what HTTP/2 handles worst)
  into shared storage. The manager dialog lists the catalog with per-model download /
  delete; a model appears in the picker once downloaded, and switching models reloads the
  engine. Selecting one **starts loading it immediately** rather than waiting for the
  first question: the load is seconds (up to ~14 s for a GPU build) and cannot be made
  cheaper, only moved somewhere the user is not watching.
- **Integrity** — each catalog entry carries Hugging Face's published `lfs.size` and
  `lfs.oid` (**SHA-256**; HF publishes no MD5). Model files live in shared storage that
  survives an uninstall, so "the file is there" is not the same as "the right file is
  there": a leftover can be truncated, half-copied, or a different build under the right
  name. So the digest gates three moments — a download skips the network only on a match,
  a finished transfer is verified *before* the `.part` is renamed into place, and an
  import whose bytes match a catalog entry is **adopted as that model** rather than
  registered as a second copy of it. Status refresh stays on the cheap length check
  (hashing several GB each time the dialog opens would stall the UI); the digest runs only
  where a wrong answer would cost a multi-GB download or a mid-chat load failure.
  `OnDeviceCatalogTest` guards the transcribed digests — one wrong character is invisible
  until a user watches a model they already have download itself again.
- **Layered ML Kit GenAI** (Gemini Nano): on AICore-capable devices (Pixel 9/10,
  Galaxy S25/S26, …) `MlKitTextService` powers a composer **"Polish draft"** action
  (rewrite/proofread). Hidden where AICore is unavailable — it can't do open-ended
  chat, so it's a bounded helper, not a chat engine.
- **Dependencies**: `com.google.ai.edge.litertlm:litertlm-android`,
  `com.google.mlkit:genai-rewriting` / `genai-summarization`, and
  `kotlinx-coroutines-guava` (ML Kit returns Guava `ListenableFuture`).

### Measured: which engine, which backend

Everything here comes out of `/probe`, which drives both engines through one report —
LiteRT-LM's own `benchmark()` and llama.cpp's `llm::LocalStats` — so a `.litertlm` and a
`.gguf` of the same model land in the same table. Warm runs, 128-token prefill, 64-token
decode, Snapdragon 8 Elite Gen 5 unless noted.

**decode, tokens/sec — the number a reader feels:**

| | | LiteRT CPU | LiteRT GPU | llama.cpp CPU |
|---|---|--:|--:|--:|
| **8 Elite Gen 5** | Gemma 4 E2B | 30.3 | **50.8** | 20.7 |
| | Qwen3 4B Instruct | 7.8 | *(no GPU build)* | **12.4** |
| **Snapdragon 865** | Gemma 4 E2B | **12.6** | 11.9 | 5.0 |

**Neither engine wins. Which one is faster depends on whose architecture the model is** —
which is why both ship and why a spec, not a global setting, picks between them.

- **Gemma E-series → LiteRT, by a lot.** The "E" is *effective*: MatFormer plus per-layer
  embeddings, an architecture Google designed for edge inference and its own runtime
  implements properly. The giveaway is on disk — the Q4_K_M GGUF is **2.89 GB against
  LiteRT's 2.41 GB int8 build**. A 4-bit file larger than an 8-bit one means llama.cpp is
  carrying weights LiteRT does not read per token, and decode is bound by bytes read per
  token, so it pays for them on every token.
- **Anything plainly dense → llama.cpp.** Qwen3 4B has no special structure to exploit, so
  the comparison is kernel quality, and that is what llama.cpp has spent years on. Same
  turn end to end (128-token prompt, 200-token answer): **34.6 s on LiteRT, 21.7 s on
  llama.cpp.**
- **The single biggest lever is not the engine — it is a compiler flag.** FEAT_I8MM alone
  takes Qwen3 4B decode from 5.3 to 12.4 tok/s; see
  [On-device llama.cpp](#on-device-llamacpp) for why a cross build loses it silently and
  what `GGML_CPU_ALL_VARIANTS` does about it.
- **On the GPU, where it exists.** LiteRT's GPU build wins prefill outright (1472 vs 241
  tok/s on the 8 Elite) and decode by 1.7×, but it **never gets a warm load**: seven runs
  across two phones, init stuck at 14–19 s while the CPU path's cache takes it to ~1 s. It
  pays ~13 s once per process to save ~3 s a turn, so it breaks even around the fifth
  message — good for a conversation, bad for a single question. It is also not always
  available: OpenCL is a vendor library, so an app targeting API 31+ must declare
  `<uses-native-library>` for it, and a device whose OEM does not publish it has no GPU
  path at all. Only Google publishes `-gpu` builds, and only for its own models, so for
  Qwen3 4B the column does not exist.
- **NPU remains untested.** `litert-community` publishes per-SoC NPU builds
  (`_qualcomm_sm8750`, `_qcs8275`); an NPU has its own memory path, so nothing above
  settles it.

**An older phone says something different, which is the point of measuring twice.** Two
things change on a Snapdragon 865, and neither follows from the 8 Elite numbers:

- **The GPU stops being a GPU.** LiteRT's GPU and CPU decode identically (11.9 vs 12.6
  tok/s) — that chip's CPU already saturates its memory controller, so a second engine has
  no bandwidth left to find. Decode is bandwidth-bound everywhere; **how much bandwidth a
  given engine can pull is a property of the chip**, not something to reason out from the
  architecture.
- **The gap between engines widens.** On Gemma 4 E2B, LiteRT is 1.5× llama.cpp on the 8
  Elite but **2.5× on the 865** (12.6 vs 5.0 tok/s; three runs gave 4.8 / 5.0 / 5.0, with
  prefill 11.9–12.0, init 3.2–4.5 s and first token ~9.4 s). Part of that is the flag: the
  865 predates FEAT_I8MM, so `GGML_CPU_ALL_VARIANTS` hands it a lower CPU variant while the
  8 Elite gets the fast one. The engine ranking holds — the *size* of the win does not
  transfer between chips.

Reading any of these numbers: phones throttle under sustained load (the 865's CPU prefill
fell 92 → 78 tok/s back-to-back, then recovered to 120 once cool), so only gaps far larger
than that spread mean anything. And the first load of a model builds a cache — the 14–18 s
that precedes a ~1–2 s steady state — so a single run of each proves nothing.

## Health data (on-device)

The app reads on-device health data and pushes it to the backend's FHIR endpoint —
the ingestion path for the phone-vendor health stores that have no server API
(Apple / Samsung / Xiaomi / …; see [src/health/README.md](../src/health/README.md)).

- **Sources** (`data/health/`): `HealthConnectSource` (Android **Health Connect** —
  every GMS device: Pixel, Samsung, Xiaomi/Mi Fitness, Honor, OPPO, vivo) and
  `HmsHealthSource` (**Huawei Health Kit**, for Huawei devices, which have HMS Core
  but no Health Connect). `HealthSourceFactory` picks one at runtime — one APK
  covers both ecosystems.
- **Flow**: read the last 7 days of steps / heart rate / sleep / weight → map each
  to a FHIR R4 `Observation` (`FhirObservation`) → `POST /fhir/Observation`
  (`HealthRepository`; the bearer token is attached by the usual interceptor).
- **Permissions**: read-only `android.permission.health.READ_*` for the four
  metrics, granted at runtime via the Health Connect permission contract (launched
  from `HealthSyncDialog`). Health Connect also needs a permissions-rationale
  activity for store approval — noted in `AndroidManifest.xml`.
- **Dependencies**: `androidx.health.connect:connect-client` + `com.huawei.hms:health`
  (Huawei maven added in `settings.gradle.kts`). The **HMS path is gated**: it
  stays inert until you add the AppGallery Connect Health Kit entitlement +
  `agconnect-services.json` + the agconnect plugin. `HealthConnectSource` works on
  GMS devices with none of that.

### Direct Bluetooth (BLE GATT) sensors

Beyond the platform stores, the app can talk **directly to a standard-profile BLE
sensor** (`data/health/ble/`) — the Android-native sibling of the desktop Qt
`BleHealth`. `BleHealthController` scans with the framework `android.bluetooth.le`
APIs, connects over GATT, subscribes to each supported measurement characteristic,
decodes it (`GattHealthCodec`), and POSTs the FHIR `Observation` to `/fhir/Observation`
— the same ingestion path as Health Connect (no new server code). No extra Gradle
dependency: BLE is in the framework. Reached from the chat overflow menu → **Bluetooth
devices** (`ui/health/BleDeviceDialog`).

- **Supported services**: Heart Rate `0x180D` → `0x2A37`, Blood Pressure `0x1810` →
  `0x2A35` (systolic/diastolic/MAP), Health Thermometer `0x1809` → `0x2A1C`. Adding
  one is a branch in `GattHealthCodec.decode` + a UUID constant. Consumer
  watches/rings use proprietary/encrypted GATT and are **not** readable here — they
  stay on the cloud vendor clients / Health Connect.
- **When to use it**: this is the fallback for standard medical sensors that have no
  companion app. Most devices still route through Health Connect first — prefer that.
- **Permissions** (requested at runtime from the dialog): `BLUETOOTH_SCAN` +
  `BLUETOOTH_CONNECT` on API 31+ (`neverForLocation`), or `ACCESS_FINE_LOCATION` on
  API 26–30 (BLE scan requires it there); declared in `AndroidManifest.xml` with a
  non-required `bluetooth_le` feature. The GATT layer serializes CCCD writes through a
  one-op-at-a-time queue (Android permits a single outstanding GATT operation).

### Legacy classic-Bluetooth HDP (Android ≤ 9)

Old **IEEE 11073 HDP** medical devices (classic Bluetooth, not BLE) can still be
ingested via `data/health/hdp/` — **basic and experimental**, and only on **Android 9
and below (API ≤ 28)**: the framework `BluetoothHealth` was deprecated in API 29 and has
no OS runtime support after that (the menu entry is hidden above API 28). `Build.VERSION`
gates it automatically. `HdpHealthController` registers a health **sink** and waits for
the device to open a channel; `Ieee11073Agent` drives the 11073-20601 exchange over the
channel FD (association → config → data) and POSTs the extracted measurements as FHIR,
same as BLE. The handshake/framing are unit-tested; the MDER measurement parse is
best-effort (single-value devices map via the sink specialization) and **not
hardware-validated** — contributions welcome. Uses the classic `BLUETOOTH` /
`BLUETOOTH_ADMIN` permissions (already declared, `maxSdkVersion=30`); no BLE/location
permission needed. Kept so the community knows legacy hardware still has a path in.

## Project layout

```
app/src/main/
├─ assets/
│  ├─ google-services.json          # Firebase project config (baked into the APK)
│  ├─ help/help-{en,zh}.md          # the /help guide — also the rendering self-check
│  └─ echarts.min.js, chart-theme.js  # chart runtime; chart-theme.js is vendored, keep byte-identical
└─ java/ai/thetahealth/mirobody/
   ├─ MainActivity.kt / MirobodyApp.kt
   ├─ di/AppContainer.kt              # Hand-rolled DI container
   ├─ data/
   │  ├─ net/                         # OkHttp/Retrofit, ApiEnvelope, interceptors, error bus
   │  ├─ auth/                        # AuthApi / AuthRepository / GoogleAuthRepository / FirebaseInitializer
   │  ├─ chat/                        # ChatApi / ChatStreamClient (SSE) / DTOs
   │  ├─ llm/                         # On-device LLM: two engines behind OnDeviceLlmEngine
   │  │                                #   LiteRtLlmEngine (.litertlm) + the llama.cpp
   │  │                                #   bridge in NativeBridge; ModelManager owns the
   │  │                                #   files, OnDeviceBenchmark measures either one
   │  ├─ health/                      # HealthSource (Health Connect / HMS), FHIR mapper, HealthRepository
   │  └─ settings/SettingsStore.kt    # DataStore (BASE_URL, token, locale, font size)
   └─ ui/                              # all Compose — there is no res/layout/ in this project
      ├─ MirobodyNavGraph.kt
      ├─ DrawerRow.kt                  # shared nav-drawer row / header / hamburger
      ├─ DialogTitle.kt                # shared dialog title + ✕ (see UI conventions)
      ├─ auth/                         # EmailScreen (+ Google button)
      ├─ chat/                         # ChatScreen, HistoryScreen, ImageViewerDialog, SlashCommand
      │                                #   reply rendering: MarkdownText, MarkdownSegments,
      │                                #   InlineMath, SvgFigure, EChartsView
      ├─ health/                       # HealthSyncDialog + HealthViewModel
      ├─ circle/                       # care circle + share-conversation dialogs
      ├─ vendor/                       # health-vendor linking dialog
      ├─ settings/                     # AppSettingsSection (the drawer's settings group) + dialogs
      └─ theme/                        # Color / Theme / Type — the real styling, not res/values
```

## Backend contract (essentials)

- Every JSON response is wrapped as `{code, message, data}` — `code == 0` means success; on non-zero the client raises `ApiException` carrying `message`. See `data/net/ApiEnvelope.kt`.
- Fetch `GET /api/agents` at startup and use the returned `code` (`Deep` / `Mix` / `Baseline`) as the `agent` field in chat requests. Do not hardcode.
- `POST /api/chat` is `text/event-stream`. Each frame is `data: {json}\n\n`. Event types handled in `data/chat/dto/SseEvent.kt`:
  - `id` — backend reply id for the assistant turn.
  - `thinking` / `reply` — incremental text deltas.
  - `queryTitle` / `queryArguments` / `queryDetail` — tool invocation lifecycle, correlated by `tool_id`.
  - `image` — an image URL (e.g. chart rendered by an agent).
  - `costStatistics` — per-turn token + cost accounting.
  - `heartbeat` / `error` / `end`.
- `GET|POST /api/history` returns `summaries[]` — `session_id`, `timestamp`, `summary`, `owned`,
  and `message_count` (see `src/chat/README.md`). `message_count` is **newer than some
  deployments**: `SessionSummary` defaults it to 0 and the row then renders the timestamp
  alone, so an older backend degrades rather than showing "0 messages".
- The backend exposes no `/docs`, no `/me`, and no refresh endpoint. The user id is decoded locally from the JWT `sub` claim.

For the full API surface see the backend source: `mirobody/chat/service.py`, `mirobody/user/user_service.py`, `mirobody/chat/adapters/http.py`.

## Embedded C++ server

This app can run the mirobody C++ server **in-process** instead of talking to a remote backend.
On launch `MainActivity` starts `MirobodyService` (a foreground service) which loads `libmirobody.so`
through `NativeBridge` and serves on `127.0.0.1:8080`. The client's default `BASE_URL` is
`http://localhost:8080` (`SettingsStore.DEFAULT_BASE_URL`), so out of the box the UI talks to the
embedded server; point it elsewhere any time via Settings → Server URL.

`libmirobody.so` is built from the repo-root [`CMakeLists.txt`](../CMakeLists.txt) (`if(ANDROID)` ->
[`src/platform/android_jni.cpp`](../src/platform/android_jni.cpp)) and needs the server's native
dependencies cross-compiled for the target ABI under `android/prebuilt/<ABI>/`.

To produce `android/prebuilt/arm64-v8a/` (OpenSSL, curl, libwebsockets, yaml-cpp, hiredis,
rapidjson, sqlite3), run the script for your OS. Both cross-compile through vcpkg's
`arm64-android` triplet, pinned to `vcpkg.json`'s baseline and the NDK pinned in
`app/build.gradle.kts`, and both take an optional ABI argument (default `arm64-v8a`).
Budget ~30-60 min for a cold build.

```bat
:: Windows (cmd, or double-click)
android\build-prebuilt.cmd
```

```sh
# macOS / Linux
./android/build-prebuilt.sh
```

Three Gradle properties steer this. Set them in `android/gradle.properties`, in Android Studio's
Settings → Build → Compiler → **Command-line Options**, or per invocation with `-P`:

| Property | Default | What it does |
| --- | --- | --- |
| `mirobody.native` | `true` | `false` forces a pure-client APK even with the prebuilts in place. Also the supported way to compile-verify Kotlin without invoking CMake — no renaming `android/prebuilt/` out of the way. |
| `mirobody.abi` | `arm64-v8a` | Which ABI the embedded server is built for. `build-prebuilt.cmd`/`.sh` takes the same names, so `build-prebuilt.cmd x86_64` + `-Pmirobody.abi=x86_64` gets the C++ server onto an **x86_64 emulator at full speed** — the emulator only keeps hardware acceleration when guest and host architectures match, so an arm64 image on an x86_64 host falls back to whole-system software emulation. |
| `mirobody.baseUrl` | `https://test.mirobody.ai` | The address the app uses before the user picks one, embedded build or not. An embedded build carries a server, but a fresh one is empty and has no account to sign in with — so it is a Settings → Backend away rather than the default. Use e.g. `http://10.0.2.2:18080` to reach a server running on the emulator's host. |

```sh
# fast UI iteration on an x86_64 emulator, no native build at all
./gradlew :app:assemblePhoneDebug -Pmirobody.native=false

# embedded server on an x86_64 emulator (after: ./android/build-prebuilt.sh x86_64)
./gradlew :app:assemblePhoneDebug -Pmirobody.abi=x86_64
```

`app/build.gradle.kts` wires the NDK/CMake build **only when `android/prebuilt/<ABI>/` exists**. Until
you run the script the app still assembles as a pure client (no `.so`); if the service starts without
the library it logs an `UnsatisfiedLinkError`, stops, and the UI keeps running.

The embedded server uses the **SQLite** backend and reads `OPENAI_API_KEY` / `GEMINI_API_KEY` /
config path from the service intent (see `MirobodyService`); supply those to enable upstream chat.

## Working in Android Studio

Things that surprise people on a fresh clone.

### There are no layout XML files

Not one — there is no `res/layout/` anywhere. The whole UI is Jetpack Compose, so the Project pane
has nothing to show under "layout" and the Layout Editor never opens. `MainActivity.setContent { }`
is the entry point; from there it is all Kotlin.

| Looking for | It is here |
| --- | --- |
| "the screens" | `ui/MirobodyNavGraph.kt` (routing) |
| chat screen | `ui/chat/ChatScreen.kt` — top bar, message list, composer and drawer all live in this one file |
| sign-in screen | `ui/auth/EmailScreen.kt` |
| dialogs | `ui/settings/`, `ui/health/`, `ui/circle/`, `ui/vendor/` |
| colours, type, theme | `ui/theme/` — **not** `res/values/themes.xml`, which only covers the launch theme |

Still XML, and visible where you would expect: `res/values/strings.xml` (+ 9 locale variants),
`res/values-night/`, `res/drawable/`, `res/xml/`.

The Compose equivalent of the Layout Editor is **Preview**: open a file with `@Preview` functions and
switch the editor to Split. `ui/DrawerRow.kt`, `ui/chat/ChatScreen.kt`,
`ui/settings/LanguageDialog.kt` and `ui/settings/FontSizeDialog.kt` carry them. They cover
self-contained components only — a whole screen cannot be previewed, because screens build their
ViewModel from `LocalAppContainer` and `AppContainer` constructs OkHttp / Retrofit / DataStore against
a real `Context`. Previewing one would mean extracting an interface for it first.

### JDK

Any JDK 17+ runs the build; Android Studio's bundled JBR is fine. There is deliberately **no pinned
daemon JVM** — a `gradle/gradle-daemon-jvm.properties` used to pin vendor `jetbrains` + version 21,
which broke every machine whose Android Studio had moved past JBR 21 and could not self-heal (Gradle
has no download URL for the JetBrains vendor, and daemon-JVM provisioning does not go through the
foojay resolver). If you run `gradle updateDaemonJvm`, do not commit the file it writes.

### SDK location

`local.properties` is gitignored; Android Studio writes your own `sdk.dir` on first sync. If your SDK
is *not* at the default `%LOCALAPPDATA%\Android\Sdk` (macOS/Linux: `~/Android/Sdk`), also export
`ANDROID_HOME` before running `build-prebuilt.*` — that script resolves the NDK itself and only falls
back to the default path.

### Emulators and ABIs

An embedded build sets `abiFilters` to the one ABI it built, so the APK **will not install on an
emulator of a different architecture**. Default is `arm64-v8a`, so on a typical x86_64 desktop:

- **Fast UI work** — `-Pmirobody.native=false`. No `abiFilters`, universal APK, runs on an x86_64
  emulator with hardware acceleration.
- **Embedded server on an emulator** — build the deps for the host architecture
  (`build-prebuilt.cmd x86_64`) and pass `-Pmirobody.abi=x86_64`. Keeps acceleration.
- **Embedded server as shipped** — a physical arm64 device.

Installing an arm64 *system image* on an x86_64 host is possible (the SDK Manager offers them up to
API 37) but rarely worth it: the emulator only hardware-accelerates when guest and host architectures
match, so an arm64 guest falls back to whole-system software emulation and is roughly an order of
magnitude slower.

`android/prebuilt/` and vcpkg's install tree are both **per-ABI**, and `build-prebuilt` only clears
the ABI it is building. Adding x86_64 does not disturb an existing arm64 tree, and switching back and
forth later is incremental — AGP keeps a separate `app/.cxx/**/<abi>/` per ABI too.

### PowerShell: quote `-P` arguments

`-Pmirobody.native=false` unquoted is split by PowerShell into `-Pmirobody` and `.native=false`, and
Gradle then fails with ``Task '.native=false' not found``. Wrap the whole argument:

```powershell
gradle :app:compilePhoneDebugKotlin "-Pmirobody.native=false"
```

## Build

### Kotlin-only APK (no C++ binding)

The native server is **optional** — `app/build.gradle.kts` wires the NDK/CMake build only when
`android/prebuilt/<ABI>/` exists *and* `mirobody.native` is not `false`. Either way out gives a pure
Kotlin client; `MirobodyService` degrades gracefully when `libmirobody.so` is absent (logs an
`UnsatisfiedLinkError`, stops, UI keeps running).

Do **not** use the `build_apk` scripts for this — their first step cross-compiles the native deps into
`prebuilt/<ABI>/` when missing, which then turns the C++ build back on. Invoke Gradle directly instead:

```bash
gradle :app:assemblePhoneDebug --no-daemon   # phone debug APK -> app/build/outputs/apk/phone/debug/
gradle :app:assembleWatchDebug --no-daemon   # watch debug APK -> app/build/outputs/apk/watch/debug/
gradle :app:assemblePhoneRelease --no-daemon # phone release build
gradle :app:installPhoneDebug --no-daemon    # install phone build on a connected device
gradle :app:installWatchDebug --no-daemon    # install watch build on a connected watch
```

The bare `assembleDebug` / `installDebug` aggregate tasks still exist but build **both** flavours —
prefer the flavour-specific tasks above. No NDK required. If you have already populated
`android/prebuilt/`, add `-Pmirobody.native=false` rather than deleting it — the prebuilts cost
30-60 min to regenerate and the switch is what they exist for.

### 16 KB page alignment

Android 15+ devices may run a 16 KB kernel page size, and the loader can only map a segment
whose alignment is a multiple of the page size — a `.so` with 4 KB-aligned LOAD segments does
not load there at all, and Play gates on it. Two halves, both now in place:

- **The APK**: `packaging.jniLibs.useLegacyPackaging = false` (already set) stores each `.so`
  uncompressed and page-aligned in the zip.
- **The ELF**: `-Wl,-z,max-page-size=16384` on the `mirobody` target in the repo-root
  `CMakeLists.txt`. NDK r27 (pinned here) still defaults to 4 KB; r28 flipped it. Without the
  flag the build succeeds and only the *install* complains — `libmirobody.so` was the one
  offender, the four prebuilt/AAR libraries were already aligned.

Check any build with the NDK's readelf — every `LOAD` line should say `0x4000`:

```bash
unzip -o app/build/outputs/apk/phone/debug/app-phone-debug.apk 'lib/arm64-v8a/*' -d /tmp/apk
"$ANDROID_NDK/toolchains/llvm/prebuilt/<host>/bin/llvm-readelf" --program-headers \
    /tmp/apk/lib/arm64-v8a/libmirobody.so | grep LOAD
```

### On-device llama.cpp

`build-llama.cmd [abi] [clean]` — `build-llama.sh` on macOS/Linux — cross-compiles llama.cpp into
`android/prebuilt/llama-sdk/<abi>/{include,lib}`, which `app/build.gradle.kts` finds by
itself and turns into `-DMIROBODY_ONDEVICE_LLM=ON -DLLAMA_CPP_DIR=...`. Delete the
directory and the app still builds: `src/llm/local.cpp` compiles a stub whose
`available()` is false.

**The instruction set is worth more than the engine choice.** Measured on an 8 Elite
Gen 5, adding FEAT_I8MM took Qwen3 4B decode from **5.3 to 12.4 tok/s — 2.3× from one
instruction set.** A cross build cannot detect this on its own: CMake's feature tests
compile *and run*, so on a cross toolchain they all "fail" and ggml silently drops its
fast kernels.

And no single `-march` can serve one APK — an i8mm binary **SIGILLs on a Snapdragon 865**,
which `minSdk 26` still allows. So the build does not choose: **`GGML_CPU_ALL_VARIANTS`**
compiles the CPU kernels seven times, one `libggml-cpu-android_armv*.so` per feature
level, each exporting `ggml_backend_score()`. At startup ggml dlopens them all, asks each
what it scores on *this* chip, and keeps the winner. `/probe` reports the winner by name
(`kernels: android_armv8.6_1`), which is the one line that tells you the fast path is
actually running.

Three things make it work, and each fails silently on its own:

- **`GGML_BACKEND_DL` needs `BUILD_SHARED_LIBS`**, so the SDK is `.so` files rather than
  the static archives the Harmony build produces — ~9.8 MB stripped in place of a 3.4 MB
  static link. That is the price of never choosing wrong.
- **`useLegacyPackaging = true`**, because ggml finds the modules by scanning a directory.
  With the modern uncompressed packaging the libraries never leave the APK and the scan
  comes back empty; unpacking them ourselves is not a way out either, since W^X blocks
  `dlopen` from an app-writable path on API 29+.
- **`CMAKE_MODULE_LINKER_FLAGS`, not just `CMAKE_SHARED_`**, for
  `-Wl,-z,max-page-size=16384`. ggml builds the variants as CMake `MODULE` libraries,
  which ignore the SHARED flags entirely — setting only those aligns `libllama.so` and
  leaves all seven variants at 4 KB, i.e. unloadable on a 16 KB-page device.

`MirobodyApp.onCreate` passes `applicationInfo.nativeLibraryDir` down through
`NativeBridge.localBackendPath`; ggml's own search looks beside the executable, which on
Android is `/system/bin/app_process`.

**Dispatch is conservative on an old kernel, and that is a real cost.** Measured on a
Meizu 16s Pro (Snapdragon 855, Android 9): `/probe` reports `kernels: android_armv8.0_1`
— the baseline — even though the Cortex-A76 in that chip *has* FEAT_DotProd. ggml scores
the variants off `AT_HWCAP`, and this device's 4.14 kernel predates `HWCAP_ASIMDDP`
(Linux 4.20), so it never advertises the bit; `/proc/cpuinfo` stops at `dcpop`. The
silicon can run those kernels and the old fixed `-march=armv8.2-a+dotprod` build did.
Dispatch gives that up in exchange for never guessing wrong on a device that genuinely
lacks the feature — the trade is deliberate, and `/probe` is where you see which side of
it a phone landed on.

### Device flavours (phone / watch)

One codebase, one `applicationId`, two `device`-dimension product flavours (`app/build.gradle.kts`):

- **phone** — the default; existing behaviour, unchanged.
- **watch** — small AOSP wearables (e.g. Android 9, ~410×502). Same full feature set and auth; only
  `BuildConfig.IS_WATCH = true`, a `-watch` version suffix, and the optional `app/src/watch/res/`
  overlay differ.

Layout adapts at **runtime**, not per flavour: `MainActivity` measures the window and publishes a
`LayoutInfo` (`ui/LayoutInfo.kt`) via `LocalLayoutInfo`. Screens read `LayoutInfo.dense` (true on the
watch flavour, or any short/narrow window) to compact spacing, content-width caps, the history drawer,
and chat-bubble widths. This means a split-screen/foldable phone window compacts too, and Compose
previews render with a sensible phone default. The flavour exists for a distinct installable build and
resource overlays; it is not required for the responsive behaviour.

### Full APK (with embedded C++ server)

One command (cross-compiles the native deps if needed, resolves/downloads Gradle, assembles the APK):

```bash
./build_apk.sh            # or: build_apk.cmd          (debug, arm64-v8a)
./build_apk.sh release    # or: build_apk.cmd release  (release build)
```

See [Embedded C++ server](#embedded-c-server) above for what the prebuilt deps are and how they're produced.

### Gradle resolution / wrapper

The build_apk scripts resolve Gradle in order `./gradlew` -> `gradle` on `PATH` -> a download of the
pinned distribution (`gradle/wrapper/gradle-wrapper.properties`, currently 9.4.1) cached locally. You
can also build from Android Studio.

The repo ships no Gradle wrapper jar/scripts, so `./gradlew` only works after you run `gradle wrapper`
once (or use the build_apk scripts / Android Studio).

On first launch the app targets whatever `mirobody.baseUrl` resolved to at build time —
`https://test.mirobody.ai` unless overridden, so a new install has somewhere it can actually sign in.
To point it elsewhere — including at the in-process server an embedded build is already running on
`http://localhost:8080` — open the nav drawer (☰) → **Backend** and enter a base URL, then sign in.