# Mirobody HarmonyOS app

> [!IMPORTANT]
> **Before opening this project in DevEco Studio — once per clone:**
>
> ```sh
> git update-index --skip-worktree harmony/build-profile.json5
> ```
>
> That file is **tracked** but takes machine-local values — the signing certificate paths
> and passwords DevEco writes for you (details in [Signing](#3-signing)) — and the flag
> lives in `.git/index`, so it does not survive a clone. Skip this and the signing block
> shows up as a committable change; set the flag **before** editing, not after.
>
> `entry/build-profile.json5` needs no such treatment: the on-device LLM SDK is found by
> repo-relative path, so nothing machine-local goes in it (see
> [On-device engine](#2-on-device-engine-optional)).

The HarmonyOS Next client — an **ArkTS / ArkUI (stage model)** app, the Huawei counterpart
of the Kotlin/Compose app under [`android/`](../android). It ships the same chat surface as
the web client in [`htdoc/`](../htdoc), and it embeds the C++ core
([`src/`](../src)) as a native module, exactly as Android loads `libmirobody.so` over JNI.

- **Bundle:** `ai.thetahealth.mirobody` (same identity as the Android package)
- **SDK:** `compatibleSdkVersion` 5.0.0(12), built with DevEco Studio 6.0 / OpenHarmony native SDK API 21
- **Devices:** `phone`, `tablet`, `2in1`
- **UI:** ArkUI declarative (ArkTS), one `@Entry` page + a nav drawer, hand-rolled markdown renderer
- **Storage:** `@kit.ArkData` preferences (config, sessions) + **ASSET Kit** (API keys)
- **Native:** NAPI bridge → `mirobody_core` (SQLite profile) → optional llama.cpp for on-device inference
- **Health:** **Health Service Kit** (`@kit.HealthServiceKit`) → FHIR Observations in the app's own SQLite
- **Permissions:** `ohos.permission.INTERNET`, `ohos.permission.FILE_ACCESS_PERSIST`

## What is different about this client

There is **no Mirobody server and no account** anywhere in this app. Where the Android / iOS /
Qt clients sign in against a backend and call `/api/chat`, this one talks straight to each
provider's own OpenAI-compatible endpoint with a key the user pastes (BYOK), or runs a model
locally. Everything that depended on a server is therefore absent by design:

| | android / ios / qt | harmony |
|---|---|---|
| Backend | `BASE_URL` + email/Google sign-in → `/api/chat` | none — no URL setting, no sign-in, no JWT |
| Provider list | `POST /api/providers` | built-in BYOK registry + the user's keys |
| History | `GET /api/history` (server) | local only ([`SessionStore.ets`](entry/src/main/ets/core/SessionStore.ets)) |
| Health data | read the phone's store → `POST /fhir` | read 华为运动健康 → the app's OWN FHIR store, on device ([Health data](#health-data)) |
| Care circle / EHR / BLE | yes | no (account- and server-shaped: sharing needs someone to share *with*) |
| Languages | 8–10 | 2 (简体中文 / English) |

See [`docs/privacy-tiers.md`](../docs/privacy-tiers.md) for the lane model this implements.

## Layout

```
harmony/
  AppScope/app.json5              bundle name / version / icon / label
  build-profile.json5             signing + products; tracked — set skip-worktree right after
                                  cloning, DevEco writes signing material into it (see Signing)
  entry/
    build-profile.json5           externalNativeOptions: abiFilters (no local paths)
    src/main/module.json5         abilities, permissions, pages
    src/main/cpp/
      CMakeLists.txt              libmirobody.so = NAPI bridge + c_api.cpp + mirobody_core
      napi_init.cpp               the bridge (cloud turns, local turns, device probes)
      membw.cpp                   DRAM bandwidth probe (-O2 scoped to this file)
      types/libmirobody/          index.d.ts — the ArkTS view of the native surface
    src/main/ets/
      core/ChatEngine.ets         one turn: router -> lane -> transport -> stream
      core/Router.ets             local vs cloud per turn (manual + heuristics)
      core/Transport.ets          BYOK direct SSE client + live model discovery
      core/NativeCore.ets        the embedded core as a cloud transport
      core/LocalModelStore.ets    GGUF references (import / download dir), persisted grants
      core/SecureKeyStore.ets     ASSET Kit, alias byok.key.<providerId>
      core/ConfigStore.ets        selection, user-added models, failure counters
      core/SessionStore.ets       local chat history (index + msg.<id>)
      core/HealthSource.ets       Health Service Kit: authorize + read the 8 metrics
      core/HealthRepository.ets   readings -> FHIR Observations -> the on-device store
      core/Markdown.ets           markdown -> RichBlock[] built for streaming
      core/RenderHost.ets         LaTeX -> SVG, ECharts -> PNG via one hidden Web
      model/HealthMetrics.ets     metric registry: the LOINC + UCUM coding per metric
      components/                 drawer, key manager, add-models, settings, health, RichMessage
      pages/Index.ets             the app; pages/NativeProbe.ets  dev probe (temporary)
    src/main/resources/rawfile/
      render/                     render.html + tex-svg.js + echarts.min.js
      sql/                        copy of res/sql/sqlite — DDL the embedded core needs
  prebuilt/                       ALL prebuilt native artifacts, gitignored, two producers:
    <abi>/                          the core's C++ deps      <- build-prebuilt.cmd / .sh
    llama-sdk/<abi>/                the on-device engine     <- build-llama.cmd / .sh
  build-prebuilt.cmd / .sh        cross-compile the core's C++ deps per ABI (vcpkg)
  build-llama.cmd  / .sh          cross-compile llama.cpp per ABI (CPU only)
  vcpkg-triplets/                 overlay triplets that make vcpkg target OHOS
  icon/                           gen_icon.py — app / layered / splash icons (see icon/README.md)
```

## Lanes and transports

```
turn ─▶ Router.selectLane ─┬─ LOCAL  ─▶ nativeLocalChat   (llama.cpp on a GGUF, in-process)
                           └─ CLOUD  ─┬─ NativeCore       (embedded C++ core, curl + agents)
                                      └─ Transport        (ArkTS SSE, POST /chat/completions)
```

The router defaults to **local** and escalates to cloud on a hard signal — long input, a code
block, attachments, or the deep-thinking toggle; guard rails (offline, no key configured) win
first. Within the cloud lane, the choice between the two transports is not a user setting: the
embedded core lists only providers whose key it holds, so *membership of the selected model in
`nativeGetProviders()` is the routing test*. The core is preferred when it serves the model
(the C++ agent pipeline and tools come for free), otherwise the ArkTS SSE client runs the turn.
Nothing above the router can tell which one answered — both emit the same event stream.

## BYOK: keys, providers, models

- **Keys** live only in the device's encrypted asset store (ASSET Kit), one asset per provider
  under `byok.key.<providerId>`. They are never sent to a Mirobody server, logged, or
  persisted anywhere else; the native lane receives them through `nativeSetConfig`, i.e. into
  process memory only.
- **Providers** ship as records in [`model/Providers.ets`](entry/src/main/ets/model/Providers.ets)
  — Google Gemini, Zhipu GLM (CN), NVIDIA NIM, OpenRouter, Groq, Cerebras — each with an
  editable `baseUrl`/`model`, so any OpenAI-compatible endpoint works.
- **Two dialogs, deliberately separate.** A key is per *provider*
  ([`ProviderManagerDialog`](entry/src/main/ets/components/ProviderManagerDialog.ets), which
  probes the key with a real request before saving it); models are added per *model*
  ([`AddModelDialog`](entry/src/main/ets/components/AddModelDialog.ets)). The composer's
  dropdown offers **only** models the user added — never the full catalog.
- **Discovery is live, nothing is baked in**: the provider's own `GET /v1/models`, joined with
  its website catalog (free flag, call volume, deprecation date) and, when the provider
  publishes one, its official deprecations table — which hides models already shut down.
- **Listed ≠ callable.** A model that answers 404/410 on this account is counted; after three
  failures it is removed from the dropdown automatically and another is selected
  ([`Index.ets`](entry/src/main/ets/pages/Index.ets) → `handleModelFailure`).

## On-device models (GGUF)

llama.cpp loads a model **by path and mmaps it**, and a multi-GB download must survive an app
uninstall — so a GGUF is never copied into app storage. It is a *reference* to a file the user
owns, picked through `DocumentViewPicker`; the grant is persisted with `@ohos.fileshare` (the
HarmonyOS analogue of an iOS security-scoped bookmark) and re-activated at launch, which is
what `ohos.permission.FILE_ACCESS_PERSIST` is for. Two acquisition paths: **import** an
existing `.gguf`, or pick a **folder** to download into (`Environment.getUserDownloadDir()`
answers 801 "device doesn't support this api" on real devices, so the user names a directory
instead). Removing a model from the app never deletes the file.

On-device models appear in the same composer dropdown as cloud models, so the picker and the
turn code carry no lane special-cases.

## Health data

华为运动健康 (Huawei Health) is a **local data source** here, not a sync client. The one
health row in the drawer opens
[`HealthDataDialog`](entry/src/main/ets/components/HealthDataDialog.ets); the path is:

```
Health Service Kit ─▶ HealthSource ─▶ FHIR Observations ─▶ nativeHealthStore
   (@kit.HealthServiceKit)                                       │
                                                     fhir_resources (app SQLite)
                                                                  │
                              family_health MCP tool ◀────────────┘  (native lane turns)
```

The last hop is the point of the whole thing: [`family_health`](../res/mcp_tools/family_health.cpp)
is compiled into this build and reads the caller's Observations, and the embedded core runs
anonymous turns as the device owner (row 1) — so once a sync has landed, the model can answer
"how did I sleep this week" from local rows. No prompt injection, and nothing about the user's
health rides along in requests where they did not ask a health question.

- **Eight metrics, all read-only** (`writeDataTypes` stays empty, so the app can never write
  into the user's Huawei Health record): daily steps · sleep duration · resting HR · heart rate
  · SpO₂ · weight · blood pressure · body temperature. Each carries a LOINC + UCUM coding from
  [`model/HealthMetrics.ets`](entry/src/main/ets/model/HealthMetrics.ets), which also documents
  what is **deliberately not mapped** and why (calories/distance: the SDK does not say kcal vs
  cal; height: no unit at all; HRV/stress: no defensible code; workouts: a round of their own).
- **Daily vs instantaneous.** Steps / sleep / resting HR are one reading per day (the Kit's
  `aggregateData` offers no daily *mean* for heart rate, so a daily HR figure would have to be
  a max or a min coded as a spot measurement — deferred instead). The rest are raw samples read
  newest-first and **capped at 200 per metric per sync**, because a week of raw heart rate is
  thousands of rows.
- **Re-syncing is safe.** Every reading gets a deterministic FHIR id
  (`hw.<metric>.<startMillis>`), and [`mirobody_health_store`](../src/mirobody.h) upserts on it,
  so overlapping windows replace their own rows instead of accumulating duplicates.
- **Gated on AppGallery Connect.** Health Service Kit hands over nothing until the app has the
  Kit enabled with its read scopes **approved** (enterprise developer, privacy policy, stated
  purpose), the device has 运动健康 installed and signed in, and the user grants the sheet. Until
  then `canIUse` / `healthStore.init` / `getAuthorizations` fail and the card reports itself
  unauthorized with the Kit's own error code — the same inert-until-entitled shape Android's
  `HmsHealthSource` has. Nothing else in the app is affected.

## The native module

`libmirobody.so` is the NAPI bridge, the C ABI ([`src/platform/c_api.cpp`](../src/platform/c_api.cpp))
and `mirobody_core` in one artifact. OHOS auto-selects the **mobile profile**: no HTTP front
door (so no libwebsockets), and the provider menu lists only what a key has been injected for.
The database backend is forced to **SQLite**; the DDL ships as `rawfile/sql/` and is
re-extracted into the sandbox on every launch (keep it in sync with
[`res/sql/sqlite/`](../res/sql/sqlite) when the schema changes).

The ArkTS-visible surface is documented in
[`cpp/types/libmirobody/index.d.ts`](entry/src/main/cpp/types/libmirobody/index.d.ts):
`nativeVersion`, `nativeSetConfig`, `nativeReloadProviders`, `nativeGetProviders`,
`nativeChat` / `nativeChatCancel`, for the local lane `nativeLocalStatus`,
`nativeLocalSetThreads`, `nativeLocalChat`, and for health data
`nativeHealthStore` / `nativeHealthRecent` — the only two that return a **Promise**,
because they hit SQLite and a week of samples is hundreds of rows (chat streams
many events and needs a channel; these produce one result). Three probes exist because the questions they
answer cannot be answered from a spec sheet: `nativeProbePath` (can native `mmap` this URI's
path?), `nativeNnrtDevices` (is the NPU reachable by a third-party app at all?), and
`nativeMemBandwidth` (the memory-wall ceiling on decode — `/proc/cpuinfo` is unreadable to an
app here, so this and `nativeLocalStatus`'s `has`/`built` split are the only way to see any of it).

## Build

### 1. Cross-compile the core's C++ dependencies (once per ABI)

```cmd
build-prebuilt.cmd arm64-v8a          :: -> harmony\prebuilt\arm64-v8a\{include,lib}
```
```sh
./build-prebuilt.sh arm64-v8a
```

Configuring the native module fails on its first `find_package` without this. It uses the
vcpkg baseline pinned in [`vcpkg.json`](../vcpkg.json) plus the overlay triplets in
[`vcpkg-triplets/`](vcpkg-triplets) — read the comments there before changing anything;
targeting OHOS through vcpkg needs three non-obvious workarounds.

### 2. On-device engine (optional)

```cmd
build-llama.cmd arm64-v8a             :: -> prebuilt\llama-sdk\arm64-v8a\{include,lib}
```
```sh
./build-llama.sh arm64-v8a            # -> prebuilt/llama-sdk/arm64-v8a/{include,lib}
```

**Running the script IS the switch** — there is nothing to configure.
`prebuilt/llama-sdk/<abi>` is resolved by repo-relative path in
[`entry/src/main/cpp/CMakeLists.txt`](entry/src/main/cpp/CMakeLists.txt), exactly as
`prebuilt/<abi>` is; build it and the lane is on, `rm -rf` it and the core compiles its stub
and the app still builds with on-device chat reporting itself unavailable. That is why
nothing machine-local lands in the tracked `entry/build-profile.json5`. `-DLLAMA_CPP_DIR`
still overrides, for an SDK assembled elsewhere via `LLAMA_SDK_DIR` — but hvigor neither
expands environment variables in that string nor tolerates spaces in it, so prefer the
default.

**CPU only** — there is no backend option. `vulkan` did build and genuinely run (it
registered an iGPU on a Kirin 9020) but lost on every axis measured and cost ~50 MB of
SPIR-V in the HAP, so the plumbing was removed rather than left switched off; the numbers,
and why the cause is structural rather than a tuning miss, are in the header of
[`build-llama.cmd`](build-llama.cmd) — re-measure against them before reviving it for a
different SoC. The script also passes `GGML_CPU_ARM_ARCH` explicitly, because a cross build
with no `-march` silently lands on baseline armv8-a — CMake's compile-and-run feature probes
cannot work when cross-compiling, so ggml quietly drops the i8mm / dotprod / fp16 / SVE
kernels the device actually has.

### 3. Signing

[`build-profile.json5`](build-profile.json5) is tracked (its committed `signingConfigs` is
empty), but DevEco writes local signing material **into this file** — cert paths under
`~/.ohos` plus encrypted store/key passwords — and rewrites it from time to time. So the
**first thing to do after cloning, before opening the project in DevEco**, is:

```sh
git update-index --skip-worktree harmony/build-profile.json5
```

The flag lives in `.git/index`, so it does **not** survive a clone — every clone must set it
again, and forgetting it fails silently, by showing the signing block as a committable
change. Do not skip this step.

Then generate a local debug signature in DevEco: Project Structure → Signing Configs →
*Automatically generate signature* (needs a Huawei account login; it registers the device
UDID). The signature is per machine anyway — the passwords DevEco writes are bound to the
install that wrote them and fail `SignHap` ("Invalid storeFile") anywhere else.

**Changing shared settings** (`products` / `buildModeSet` / `modules`) means lifting the
flag, committing, and setting it again:

```sh
git update-index --no-skip-worktree harmony/build-profile.json5
# edit, commit (make sure signingConfigs stays [] in the commit), then:
git update-index --skip-worktree harmony/build-profile.json5
```

**When a `git pull` aborts on this file.** Any upstream change to those shared settings lands
on every clone that set the flag, as:

```
error: Your local changes to the following files would be overwritten by merge:
        harmony/build-profile.json5
```

`git status` and `git diff` both report clean — skip-worktree tells *them* not to look at the
working tree, while merge still refuses to clobber it — so there is nothing to stash and the
message reads as a lie. Lift the flag, keep your signing block aside, merge, put it back:

```sh
cp harmony/build-profile.json5 /tmp/signing-backup.json5   # outside the repo, so it stays untracked
git update-index --no-skip-worktree harmony/build-profile.json5
git checkout -- harmony/build-profile.json5
git pull
# paste your signingConfigs block back from the backup, then re-hide it:
git update-index --skip-worktree harmony/build-profile.json5
```

`git ls-files -v harmony/ | grep -v '^H '` lists every flag actually set, which is worth a look
first: an old clone may still carry one on `entry/build-profile.json5` from when that file held
a machine-local `-DLLAMA_CPP_DIR`. It no longer does, so drop that one with `--no-skip-worktree`
rather than restoring it — see [Machine-local, not in git](#machine-local-not-in-git).

`local.properties` (`sdk.dir` / `nodejs.dir`) is likewise gitignored and must point at your
own DevEco install.

### 4. Build the HAP

DevEco Studio is the normal path. From the command line, with DevEco's bundled hvigor and no
IDE:

```sh
# DEVECO_HOME if you set it, else the same default the build scripts use. cygpath -u
# because PATH below needs a POSIX path: a `C:\...` entry would split on its own colon.
DES="$(cygpath -u "${DEVECO_HOME:-$PROGRAMFILES/Huawei/DevEco Studio}")"
export PATH="$DES/tools/node:$PATH"
export DEVECO_SDK_HOME="$DES/sdk"
cd harmony
"$DES/tools/node/node.exe" "$DES/tools/hvigor/bin/hvigorw.js" \
  --mode module -p product=default -p buildMode=debug assembleHap --no-daemon
```

Output: `entry/build/default/outputs/default/entry-default-signed.hap`. The native stages to
look for in the log are `BuildNativeWithNinja` → `ProcessLibs` → `DoNativeStrip`; confirm
packaging by finding `libs/<abi>/libmirobody.so` inside the HAP. Only `arm64-v8a` is built —
add `x86_64` to `abiFilters` (and run `build-prebuilt` for it) to also target the emulator.

If a DevEco reinstall makes `SignHap` fail with `Invalid storeFile`, the inline passwords are
bound to the old install's key: re-tick *Automatically generate signature* to rewrite the
block. Everything before `SignHap` succeeding already proves the SDK and node paths are good.

## Rendering

The chat list holds **no webview**. [`Markdown.ets`](entry/src/main/ets/core/Markdown.ets)
parses streaming text into blocks with offset-derived stable ids (so ArkUI's `ForEach` diff
leaves already-rendered blocks alone) and a `closed` flag (so half a `$$` formula or a partial
ECharts option is never handed to a renderer). Heavy blocks go to
[`RenderHost.ets`](entry/src/main/ets/core/RenderHost.ets): one hidden `Web` component runs
MathJax and ECharts from `rawfile/render/`, hands **SVG/PNG back as strings** — no surface
snapshotting — and the results are written as files that the list draws as native `Image`s and
that double as a cross-launch disk cache. Failures are cached too, or a formula MathJax
rejects would re-queue on every rebuild.

## UI conventions

One hamburger opens everything: the left [`HistoryDrawer`](entry/src/main/ets/components/HistoryDrawer.ets)
is the app's only menu — new chat / incognito, the session list as the sole scrolling band, and
a pinned footer with Language / Font size / Appearance. Sessions are local
(`chat_sessions` preferences: a small index plus one `msg.<id>` blob per session, oldest
trimmed past `MAX_SESSIONS`), so closing the app no longer discards a conversation.

`pages/NativeProbe.ets` ("Native probe" in the menu) is a **temporary** developer page: init
the core and list what it can run, run one real turn with arrival offsets, exercise cancel,
and read the device probes. Remove it once the native transport has proven out on devices.

## Machine-local, not in git

`build-profile.json5`'s signing block (tracked file, hidden behind skip-worktree — see
Signing), `prebuilt/` (**everything** prebuilt: `<abi>/` cross-compiled deps and
`llama-sdk/<abi>/` the on-device engine), `.llama-build/` (~24 MB llama.cpp build cache),
`local.properties`, `oh_modules/`, `**/build`, `.hvigor/`, `.cxx/`.

All of it is gitignored, in-tree, and re-derivable by re-running the script that made it —
which is also what `git clean -xdf` costs you here. The two producers never collide:
`build-prebuilt` replaces only its own `prebuilt/<abi>`. `LLAMA_SDK_DIR` can move the llama
SDK out of the tree, at the price of having to name it with `-DLLAMA_CPP_DIR` in the tracked
`entry/build-profile.json5`; the default exists so that no path ever has to go there.
Nothing else in `entry/build-profile.json5` is machine-local, so it needs no skip-worktree.
