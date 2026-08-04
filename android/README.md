# Mirobody Android

Android client for [mirobody](https://github.com/thetahealth/mirobody). The primary surface is **chat with Agents** (DeepAgent / MixAgent / BaselineAgent), streamed from the backend's `/api/chat` over Server-Sent Events.

- **Package:** `ai.thetahealth.mirobody`
- **minSdk:** 26 / **targetSdk:** 35 / **JDK:** source+target 17; the build itself runs on any JDK 17+ (Android Studio's bundled JBR is fine — there is no pinned daemon JVM)
- **UI:** Jetpack Compose + Material 3 + Navigation
- **Networking:** OkHttp (with SSE) + Retrofit + kotlinx.serialization
- **Storage:** DataStore (Preferences)
- **Auth:** email-code (primary) + Firebase Auth for Google sign-in
- **Markdown:** Markwon (core / tables / strikethrough / html / linkify / latex)

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
  - Incremental `thinking` / `reply` rendering with Markdown (Markwon).
  - Expandable tool-call cards driven by `queryTitle` / `queryArguments` / `queryDetail`.
  - Inline images from `image` events, tap to open the fullscreen viewer with save-to-gallery (`ui/chat/ImageViewerDialog`).
  - Per-turn cost statistics dialog from `costStatistics`.
  - **On-device option**: run a model locally and offline — pick a downloaded model (**Gemma 4**, **Qwen**, …) shown as **"&lt;name&gt; · On-device"**, or open the on-device AI manager to download one (see *On-device LLM* below).
- Session history with per-row delete, lazily loaded — `/api/history` is not called until the navigation drawer is opened (`ui/chat/HistoryScreen`).
- Server URL configuration with presets (`ui/settings/BaseUrlScreen`)
- Language and font-size preferences (`ui/settings`)
- On-device health sync (Settings → **Sync health data**, `ui/health/HealthSyncDialog`)

## On-device LLM (private chat)

Alongside the server's agents, the provider picker exposes an **on-device AI manager**
plus any **downloaded on-device models** — **Gemma 4 E2B**, **Qwen2.5 1.5B**, … shown
as **"&lt;name&gt; · On-device"** — chat that runs entirely on the phone (no network,
no server), emitting the same `reply` stream so the chat UI is unchanged (`data/llm/`).
It stays available even when the server is unreachable.

- **Engine** (`LiteRtLlmEngine`): **LiteRT-LM** (`com.google.ai.edge.litertlm`),
  model-agnostic over the catalog in `OnDeviceModel` (LiteRT-LM `.litertlm` files, from
  the Apache-2.0 `litert-community` HF org). Weights are **not bundled** — `ModelManager`
  downloads each on demand from Hugging Face (resumable, with progress) into app-private
  storage. The manager dialog lists the catalog with per-model download / delete; a model
  appears in the picker once downloaded, and switching models reloads the engine.
- **Layered ML Kit GenAI** (Gemini Nano): on AICore-capable devices (Pixel 9/10,
  Galaxy S25/S26, …) `MlKitTextService` powers a composer **"Polish draft"** action
  (rewrite/proofread). Hidden where AICore is unavailable — it can't do open-ended
  chat, so it's a bounded helper, not a chat engine.
- **Dependencies**: `com.google.ai.edge.litertlm:litertlm-android`,
  `com.google.mlkit:genai-rewriting` / `genai-summarization`, and
  `kotlinx-coroutines-guava` (ML Kit returns Guava `ListenableFuture`).

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
├─ assets/google-services.json     # Firebase project config (baked into the APK)
└─ java/ai/thetahealth/mirobody/
   ├─ MainActivity.kt / MirobodyApp.kt
   ├─ di/AppContainer.kt              # Hand-rolled DI container
   ├─ data/
   │  ├─ net/                         # OkHttp/Retrofit, ApiEnvelope, interceptors, error bus
   │  ├─ auth/                        # AuthApi / AuthRepository / GoogleAuthRepository / FirebaseInitializer
   │  ├─ chat/                        # ChatApi / ChatStreamClient (SSE) / DTOs
   │  ├─ llm/                         # On-device LLM: LiteRtLlmEngine, ModelManager, MlKitTextService
   │  ├─ health/                      # HealthSource (Health Connect / HMS), FHIR mapper, HealthRepository
   │  └─ settings/SettingsStore.kt    # DataStore (BASE_URL, token, locale, font size)
   └─ ui/                              # all Compose — there is no res/layout/ in this project
      ├─ MirobodyNavGraph.kt
      ├─ DrawerRow.kt                  # shared nav-drawer row / header / hamburger
      ├─ auth/                         # EmailScreen (+ Google button)
      ├─ chat/                         # ChatScreen, HistoryScreen, MarkdownText, ImageViewerDialog
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
| `mirobody.baseUrl` | see below | The address the app uses before the user picks one. Defaults to `http://localhost:8080` for an embedded build (that is where the in-process server listens) and `https://test.mirobody.ai` for a pure client, which would otherwise boot onto a dead localhost. Use e.g. `http://10.0.2.2:18080` to reach a server running on the emulator's host. |

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
`http://localhost:8080` for an embedded build, `https://test.mirobody.ai` for a pure client. To point
it elsewhere, open the nav drawer (☰) → **Backend** and enter a base URL, then sign in.