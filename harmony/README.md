# Mirobody HarmonyOS app

> [!IMPORTANT]
> **Before opening this project in DevEco Studio — once per clone:**
>
> ```sh
> git update-index --skip-worktree harmony/build-profile.json5
> ```
>
> DevEco writes your local signing certificate paths and passwords into that **tracked**
> file, and the flag lives in `.git/index`, so it does not survive a clone. Skip this and
> the signing material shows up as a committable change. Details in [Signing](#3-signing).

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
| Health / care circle / BLE | yes | no (they are server- and account-shaped features) |
| Languages | 8–10 | 2 (简体中文 / English) |

See [`docs/privacy-tiers.md`](../docs/privacy-tiers.md) for the lane model this implements.

## Layout

```
harmony/
  AppScope/app.json5              bundle name / version / icon / label
  build-profile.json5             signing + products; tracked — set skip-worktree right after
                                  cloning, DevEco writes signing material into it (see Signing)
  entry/
    build-profile.json5           externalNativeOptions: abiFilters + -DLLAMA_CPP_DIR
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
      core/Markdown.ets           markdown -> RichBlock[] built for streaming
      core/RenderHost.ets         LaTeX -> SVG, ECharts -> PNG via one hidden Web
      components/                 drawer, key manager, add-models, settings, RichMessage
      pages/Index.ets             the app; pages/NativeProbe.ets  dev probe (temporary)
    src/main/resources/rawfile/
      render/                     render.html + tex-svg.js + echarts.min.js
      sql/                        copy of res/sql/sqlite — DDL the embedded core needs
  build-prebuilt.cmd / .sh        cross-compile the core's C++ deps per ABI  -> prebuilt/
  build-llama.cmd  / .sh          cross-compile llama.cpp -> an SDK dir the app links
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
`nativeChat` / `nativeChatCancel`, and for the local lane `nativeLocalStatus`,
`nativeLocalSetThreads`, `nativeLocalChat`. Three probes exist because the questions they
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
build-llama.cmd arm64-v8a cpu         :: -> D:\opt\llama-sdk-ohos-arm64-v8a-cpu
```
```sh
./build-llama.sh arm64-v8a cpu        # -> ~/opt/llama-sdk-ohos-arm64-v8a-cpu
```

`-DLLAMA_CPP_DIR` in [`entry/build-profile.json5`](entry/build-profile.json5) is the **only**
switch for the on-device lane: absent, the core compiles its stub and the app still builds,
with on-device chat reporting itself unavailable. That path is machine-local — expect to
repoint it rather than inherit it.

**Use the `cpu` backend.** The `vulkan` one builds and genuinely runs (it registers an iGPU),
but lost on every axis measured on a Kirin 9020 and costs ~50 MB of SPIR-V in the HAP; the
numbers, and why the cause is structural rather than a tuning miss, are in the header of
[`build-llama.cmd`](build-llama.cmd). The script also passes `GGML_CPU_ARM_ARCH` explicitly,
because a cross build with no `-march` silently lands on baseline armv8-a — CMake's
compile-and-run feature probes cannot work when cross-compiling, so ggml quietly drops the
i8mm / dotprod / fp16 / SVE kernels the device actually has.

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

`local.properties` (`sdk.dir` / `nodejs.dir`) is likewise gitignored and must point at your
own DevEco install.

### 4. Build the HAP

DevEco Studio is the normal path. From the command line, with DevEco's bundled hvigor and no
IDE:

```sh
DES="/d/Huawei/DevEco Studio"
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
Signing), `prebuilt/` (cross-compiled deps), `.llama-build/` (~24 MB llama.cpp build cache),
`local.properties`, `oh_modules/`, `**/build`, `.hvigor/`, `.cxx/`. The assembled llama.cpp
SDK lives outside the repo entirely (`D:\opt\llama-sdk-ohos-*`, `~/opt/...`).

One tracked file still carries a machine-local value: `entry/build-profile.json5`'s
`-DLLAMA_CPP_DIR`. It is a path, not a secret, but it will show up dirty on every other
machine — repoint it and leave it uncommitted.
