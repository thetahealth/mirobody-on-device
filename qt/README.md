# Mirobody Qt desktop client

A standalone **Qt Quick (QML)** desktop client for a running mirobody server — the native
counterpart of the web client in [`res/htdoc`](../res/htdoc). It speaks the same HTTP +
Server-Sent-Events API and links **nothing** from `mirobody_core`; a pure API client built
on Qt's own networking stack.

## Features

Full parity with the web client's main flow:

| Feature | How it works |
|---|---|
| Email one-time-code sign-in | `POST /email/login` → `/email/verify`; login.js's staircase — a valid address (`*@*.*`) unlocks **Send code**, a send unlocks the code field and starts the 60s cooldown (editing the address re-locks it), six digits unlock **Sign in** and submit on their own; token persisted via `QSettings` |
| Chat — *agent* mode | `POST /api/chat` SSE: `reply` / `thinking` / `costStatistics` / `error` events (+ live thinking block, cost footer) |
| Chat — *proxy* mode | `{model, messages, stream}` → OpenAI-style `choices[].delta.content` (+ resumable `session_id` chunk) |
| Provider picker | `POST /api/providers`, selection restored/persisted |
| **On-device private LLM** *(optional)* | each registered GGUF appears in the provider picker as **"&lt;name&gt; · On-device"**; runs locally (offline, no server) via llama.cpp, emitting the same reply stream. Manage models in **⚙ → On-device AI**. See [On-device LLM](#on-device-llm-optional) |
| Direct BLE health sensors | `BleHealth` (Qt `QLowEnergyController`) decodes GATT/IEEE-11073 → FHIR `Observation` → `POST /fhir/Observation` |
| Local persistence | transcript mirrored to `conversation-<userid>.json` under `AppDataLocation` (web client's IndexedDB stand-in) |
| History drawer | `GET /api/history`, `POST /api/history/delete` |
| Settings | backend URL (presets), language (all 10, rides each request + localises UI), font size |
| Rendering | Markdown assistant replies; RTL mirroring for Arabic/Hebrew |

**BLE detail** — standard-profile services only: Heart Rate `0x180D`, Blood Pressure `0x1810`,
Health Thermometer `0x1809` (adding one = a row in the decode dispatch in
[`blehealth.cpp`](blehealth.cpp)). Consumer watches/rings use proprietary/encrypted GATT and stay
on the cloud vendor clients. Reached via **⚙ → Bluetooth devices** when signed in. See
[`src/health/README.md`](../src/health/README.md) → *Direct Bluetooth devices*.

**Intentionally omitted:**

- **Social sign-ins** (Google/Apple/WeChat/GitHub/X) — browser-SDK popup/OAuth flows with no
  native Qt equivalent; email login reaches every account, so the client ships email-only.
- **KaTeX math** — Markdown is rendered, math is shown as source.

## Build

Off by default (built only with `MIROBODY_BUILD_QT=ON`, never on mobile). Needs **Qt 6.5+**
(Core, Gui, Qml, Quick, QuickControls2, Network, **Bluetooth**).

**Toolchain — do not mix compilers** (a MinGW build cannot link MSVC libraries, and vice-versa):

| OS | Compiler / Qt kit |
|---|---|
| Windows | **MSVC** — `msvc2022_64` kit inside `vcvarsall.bat amd64` (matches `build.cmd` and llama.cpp's Windows build) |
| Linux / macOS | system **GCC/Clang** (Qt's GCC/Clang kit) |

**Easiest — the wrapper scripts** (in this `qt/` dir). They generate the tiny
`add_subdirectory(qt)` wrapper (qt/ has no `project()` of its own), build standalone (no
`mirobody_core` deps), and output to `build-qt/app/mirobody_qt.exe` at the repo root:

```cmd
build-qt.cmd             :: -> build-qt\app\mirobody_qt.exe
build-qt.cmd deploy      :: + windeployqt, so the exe is double-clickable
```
```sh
./build-qt.sh            # Linux / macOS
```

| Env override | Meaning |
|---|---|
| `QT_ROOT` | Qt install root to scan (Windows; default `%SystemDrive%\Qt`) |
| `QT_PREFIX` | the kit itself, skipping the scan — e.g. `<qt-root>\6.11.1\msvc2022_64` |
| `VS_DIR` | Visual Studio root (Windows) |
| `NINJA` | `ninja.exe` path (Windows; default `%QT_ROOT%\Tools\Ninja`, else the one vcvars puts on PATH) |

Windows installs Qt outside `%SystemDrive%` often enough that the scan misses it — if yours
is elsewhere, `setx QT_ROOT <your-qt-root>` once and forget about it.

Keep the exe next to its generated `Mirobody/` QML module dir (loaded from disk) — don't
relocate it away from that directory.

**Or from the top-level build:**

```sh
cmake -B build-qt -S . -DMIROBODY_BUILD_QT=ON -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/<kit>
cmake --build build-qt --target mirobody_qt
```

### On-device LLM (optional)

Runs a local GGUF model **fully locally via [llama.cpp](https://github.com/ggml-org/llama.cpp)** —
the same runtime the Electron desktop client uses (the mobile clients stay on LiteRT-LM `.litertlm`).
The engine is **model-agnostic**: any GGUF works — **Gemma, Qwen, Llama, Phi, Mistral, …** — using
each model's own chat template (with a per-arch fallback for gemma4). Off by default: on-device
providers appear but are inert until built with a backend. `build-qt` clones + builds llama.cpp for
you (pure CMake, a few minutes, no TensorFlow) and **caches the SDK per backend**.

> **`build-qt.cmd check`** — detect this machine's GPU + installed SDKs and recommend a backend.

**Backend token** (giving one enables the on-device engine):

| Token | Target | Prerequisite |
|---|---|---|
| `cpu` | portable CPU baseline (`GGML_NATIVE=OFF`) | — |
| `avx2` | CPU tuned to this machine (`GGML_NATIVE=ON`) | — |
| `vulkan` | any GPU — Intel Arc / AMD / NVIDIA | Vulkan SDK |
| `cuda` | NVIDIA GPU (fastest on NVIDIA) | CUDA Toolkit |

```cmd
build-qt.cmd vulkan deploy    :: on-device via GPU (Vulkan)
build-qt.cmd cuda deploy      :: on-device via NVIDIA GPU (CUDA)
```

The backend is the **only** build-time choice — build-qt never downloads a model. **Models are a
runtime concern**: manage a **list** in **⚙ → On-device AI** (add any GGUF — paste a Gemma / Qwen /
Llama URL, or point at a local file — no rebuild), and pick which to use from the model dropdown
above the chat box, where each shows as `<name> · On-device`. The app ships a sensible default
(Gemma E4B Q4_0). `build-qt.sh` mirrors the tokens on Linux/macOS.

**Direct CMake equivalent** (if you drive CMake yourself): `-DMIROBODY_ONDEVICE_LLM=ON
-DLLAMA_CPP_DIR=<sdk>` — point `LLAMA_CPP_DIR` at a prebuilt llama.cpp SDK (`include/`, `lib/`,
`bin/`) to skip the build. Packagers can change the shipped default model with
`-DMIROBODY_GGUF_REPO=… -DMIROBODY_GGUF_FILE=…` (optional; defaults live in
`modeldownloader.cpp`). The runtime libs must sit next to the exe (build-qt copies them); `clean`
is needed to turn the engine back OFF.

## Run

Launch `mirobody_qt` → **⚙ → Backend** → point it at a mirobody server (default
`http://127.0.0.1:8080`, matching `config.yml`'s `HTTP_PORT`). Sign in with email + code — enable
demo codes by uncommenting `EMAIL_PREDEFINE_CODES` in the server's `config.yml` (off by default),
e.g. `demo1@mirobody.ai` / `777777`.

**On-device chat** (if built with a backend) — pick a `… · On-device` model from the dropdown
above the composer; it runs offline, no server needed. Add/download/remove models in
**⚙ → On-device AI**; selecting a model that isn't downloaded yet opens that manager.

**Bluetooth permission** — BLE scanning is OS-gated:

- **macOS**: needs `NSBluetoothAlwaysUsageDescription` (Info.plist); `BleHealth` requests
  `QBluetoothPermission` on first scan. `MACOSX_BUNDLE` is already set on the target.
- **Windows / Linux**: just need Bluetooth enabled — no per-app grant.

## Layout

```
qt/
  CMakeLists.txt           Qt6 Quick app + QML module (Mirobody)
  build-qt.cmd / .sh       build wrapper (this dir); see Build above
  main.cpp                 engine bootstrap; exposes AppController as `app`
  apiclient.{hpp,cpp}      HTTP envelope + SSE streaming (net.js analogue) + raw FHIR POST
  chatmodel.{hpp,cpp}      QAbstractListModel of the transcript
  blehealth.{hpp,cpp}      BLE GATT sensor scan/connect -> FHIR Observation ingestion
  appcontroller.{hpp,cpp}  settings, login, providers, streaming, persistence
  modeldownloader.{hpp,cpp}  on-device model registry (remote downloads + local files), persisted
  locallmengine.{hpp,cpp}  on-device LLM engine — any GGUF (llama.cpp; stub unless enabled)
  qml/
    Main.qml               top bar + login/chat loader + shared dialogs
    LoginPage.qml  ChatPage.qml  MessageDelegate.qml
    HistoryDrawer.qml      the app's only menu: history + health + app settings + account
    CostDialog.qml  BackendDialog.qml  BleDialog.qml  LanguageDialog.qml  FontDialog.qml
    ConfirmDialog.qml
    Theme.qml              singleton: Material 3 colour scheme (mirrors config.js)
    I18n.qml               singleton: reactive i18n facade
    strings.js             UI string table (verbatim from htdoc/src/i18n.js; regenerate if it changes)
```
