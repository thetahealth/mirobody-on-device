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
| Chat — *agent* mode | `POST /api/chat` SSE: `reply` / `thinking` / `costStatistics` / `chart` / `image` / `queryTitle`+`queryArguments`+`queryDetail` / `error` events (+ live thinking block, tool-call cards, cost footer) |
| Chat — *proxy* mode | `{model, messages, stream}` → OpenAI-style `choices[].delta.content` (+ resumable `session_id` chunk) |
| Provider picker | `POST /api/providers`, selection restored/persisted |
| **On-device private LLM** *(optional)* | each registered GGUF appears in the provider picker as **"&lt;name&gt; · On-device"**; runs locally (offline, no server) via llama.cpp, emitting the same reply stream. Manage models in **⚙ → On-device AI**. See [On-device LLM](#on-device-llm-optional) |
| Direct BLE health sensors | `BleHealth` (Qt `QLowEnergyController`) decodes GATT/IEEE-11073 → FHIR `Observation` → `POST /fhir/Observation` |
| Local persistence | transcript mirrored to `conversation-<userid>.json` under `AppDataLocation` (web client's IndexedDB stand-in) |
| History drawer | `GET /api/history`, `POST /api/history/delete` |
| Settings | backend URL (presets), language (all 10, rides each request + localises UI), font size |
| Rendering | Markdown (tables, lists, code) and ```` ```svg ```` figures; RTL mirroring for Arabic/Hebrew. **LaTeX math and ECharts charts are written but do not render yet** and fall back to source — see [Math and charts](#math-and-charts-not-rendering-yet-) |
| Slash commands | `/help` (the built-in guide), `/new`, `/incognito` — palette opens on a lone `/` |

**BLE detail** — standard-profile services only: Heart Rate `0x180D`, Blood Pressure `0x1810`,
Health Thermometer `0x1809` (adding one = a row in the decode dispatch in
[`blehealth.cpp`](blehealth.cpp)). Consumer watches/rings use proprietary/encrypted GATT and stay
on the cloud vendor clients. Reached via **⚙ → Bluetooth devices** when signed in. See
[`src/health/README.md`](../src/health/README.md) → *Direct Bluetooth devices*.

**Intentionally omitted:**

- **Social sign-ins** (Google/Apple/WeChat/GitHub/X) — browser-SDK popup/OAuth flows with no
  native Qt equivalent; email login reaches every account, so the client ships email-only.
- **Attachments** — no paperclip; the composer sends text only.

## Reply rendering

A reply is not only prose. [`ReplyBody.qml`](qml/ReplyBody.qml) splits it into the pieces that
need different renderers — prose, a ```` ```svg ```` figure, a ```` ```echarts ```` chart — re-splitting on
every token as it streams, so an **unclosed** fence stays source: half an SVG is not a figure,
and a block becomes one the moment its closing fence lands. The splitter and the `$…$`-vs-currency
rule live in [`qml/markdown.js`](qml/markdown.js), ported from Android's `MarkdownSegments.kt`
and `InlineMath.kt` so the clients agree on the judgement calls.

Prose reaches the screen as rich text rather than `Text.MarkdownText`. The conversion is still
`QTextDocument` — the same importer `MarkdownText` uses — so tables, lists and code blocks render
as they always did; going through HTML was meant to let a rendered formula be spliced into a
paragraph as an `<img>`.

### Math and charts: not rendering yet ⚠️

The machinery is written and the design is harmony's ([`renderhost.cpp`](renderhost.cpp) ports
`core/RenderHost.ets`: [`render/render.html`](render/render.html) runs MathJax + ECharts in a
hidden [`WebEngineView`](qml/RenderHostView.qml), results are cached as
`<hash>-<w>x<h>.svg|png` under `AppDataLocation/render-cache` and drawn as plain `Image`s, so the
message list never holds a web view). **It does not work on this platform yet**, and formulas and
charts fall back to their source. Two independent blockers, both measured:

1. **An `<img>` inside `Text.RichText` sends QQuickText into an endless relayout.** One inline
   formula in one paragraph pegs a core and the window stops answering — no binding-loop warning,
   nothing in any log. Measured per block kind: prose, tables, code, ` ```svg ` and ` ```echarts `
   all idle at ~0.1 core; a single inline formula sits at 1.00. Removing the `Loader` that wrapped
   the `Text` (it assigned a height back, a plausible cause) changed nothing. So the `<img>`
   splicing is disabled in [`ReplySegment.qml`](qml/ReplySegment.qml) — inline math shows as
   `$BMI = w/h^2$`. A fix needs a mechanism that is not a rich-text image.
2. **The hidden `WebEngineView` never runs the page** — `loadingChanged` reports
   `LoadFailedStatus` with an empty error string and `runJavaScript` never calls back. Qt's own
   `qml` tool runs a `WebEngineView` with V8 on the same machine, so WebEngine itself is fine
   here; something about this embedding is not. (A standalone `QWebEnginePage` is worse: its
   renderer process dies with `0xC0000409` inside `Qt6WebEngineCore.dll` as soon as a page needs
   V8 — a plain HTML file loads, anything with a `<script>` does not. `--single-process` gets the
   page running but then wedges the UI thread, which is why it is not used.)

What DOES render, verified end to end: markdown prose, tables, code blocks, and ` ```svg `
figures (native via QtSvg — a 420×120 diagram writes its file and draws). The fence splitter and
the `$…$`-vs-currency rule are unit-tested (`ctest`), including the case where a `$` before a
price must not swallow the rest of the reply.

An SVG figure (```` ```svg ````) needs no browser — Qt rasterizes it — but the source is model-authored, so it
is **validated and refused**, never rewritten: no external `href`/`src`, no DOCTYPE/ENTITY, no
`<script>`/`<foreignObject>`/`<use>`, no `on*=` handler. A refused (or unrenderable) figure falls
back to showing its XML.

Every block that cannot be drawn falls back to its source — which is also, for now, what a build
**without Qt WebEngine** looks like, and what a build with it looks like too.

## Build

Off by default (built only with `MIROBODY_BUILD_QT=ON`, never on mobile). Needs **Qt 6.5+**
(Core, Gui, Qml, Quick, QuickControls2, Network, **Bluetooth**, **Svg**), and optionally
**WebEngineQuick** — see [Math and charts](#math-and-charts-optional).

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

### Math and charts (optional)

`MIROBODY_RICH_RENDER` is **ON by default** and needs Qt's **WebEngine** component (install it
from the Qt Maintenance Tool if your kit lacks it). Configure reports which way it went:

```
-- mirobody_qt: LaTeX + ECharts rendering ON (Qt WebEngine)
-- mirobody_qt: Qt WebEngine not found -- formulas and charts will show their source. …
```

Missing WebEngine is **not an error** — the build proceeds and those two block kinds render as
source. Turn it off deliberately with `-DMIROBODY_RICH_RENDER=OFF`; nothing else in the client
depends on it.

It adds ~3 MB to the binary: `tex-svg.js` (MathJax) and `echarts.min.js`, read straight out of
[`harmony/entry/src/main/resources/rawfile/render/`](../harmony/entry/src/main/resources/rawfile/render/)
rather than copied here — that is the repo's home for the offscreen-render assets and harmony
loads the same files for the same two jobs. `deploy` (windeployqt) bundles the WebEngine runtime,
including `QtWebEngineProcess`, so a deployed build renders without Qt installed.

### On-device LLM (optional)

Runs a local GGUF model **fully locally via [llama.cpp](https://github.com/ggml-org/llama.cpp)** —
the same runtime the Electron desktop client uses, and the one of Android's two engines that reads
GGUF. The engine is **model-agnostic**: any GGUF works — **Gemma, Qwen, Llama, Phi, Mistral, …** — using
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
runtime concern**, managed in **⚙ → On-device AI**:

| Model | Download | RAM |
|---|---|---|
| Qwen3.5 2B (Q4_K_M) | 1.2 GB | 4 GB+ |
| Qwen3.5 4B (Q4_K_M) | 2.6 GB | 8 GB+ |
| Gemma 4 E2B (Q4_K_M) | 2.9 GB | 6 GB+ |
| Gemma 4 E4B (Q4_K_M) | 4.6 GB | 8 GB+ |

The same four Android offers, as GGUF — Android runs the Gemma pair on LiteRT-LM instead, because
Google's own runtime reads the E-series' MatFormer layout faster on a phone; on a desktop there is
one engine and no reason for a second format. Each row downloads in place, and **clicking a
downloaded model picks it**. Anything else works too (llama.cpp is model-agnostic — Llama, Phi,
Mistral, …): paste a GGUF URL or point at a file you already have, and it joins the list under
**Your models**. Every downloaded model also appears in the dropdown above the chat box as
`<name> · On-device`. `build-qt.sh` mirrors the tokens on Linux/macOS.

**Direct CMake equivalent** (if you drive CMake yourself): `-DMIROBODY_ONDEVICE_LLM=ON
-DLLAMA_CPP_DIR=<sdk>` — point `LLAMA_CPP_DIR` at a prebuilt llama.cpp SDK (`include/`, `lib/`,
`bin/`) to skip the build. The runtime libs must sit next to the exe (build-qt copies them);
`clean` is needed to turn the engine back OFF.

## Run

Launch `mirobody_qt` → **⚙ → Backend** → point it at a mirobody server (default
`http://127.0.0.1:8080`, matching `config.yml`'s `HTTP_PORT`). Sign in with email + code — enable
demo codes by uncommenting `EMAIL_PREDEFINE_CODES` in the server's `config.yml` (off by default),
e.g. `demo1@mirobody.ai` / `777777`.

**On-device chat** (if built with a backend) — pick a `… · On-device` model from the dropdown
above the composer; it runs offline, no server needed. Download, pick and remove models in
**⚙ → On-device AI**, which the picker's last entry also opens.

**Slash commands** — type `/` in the composer to open the palette (the same three the web and
Android clients offer, word for word): `/help` renders the built-in guide, `/new` starts a fresh
conversation, `/incognito` toggles privacy mode. A command never reaches a model, and its answer
is flagged local — it is not saved and costs nothing in the next turn's context. `/help` reads
`htdoc/static/help/help-{en,zh}.md`, compiled into the binary as a Qt resource, so the three
clients answer it with one document.

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
  modeldownloader.{hpp,cpp}  on-device model registry — built-in GGUF catalog + the user's own
  locallmengine.{hpp,cpp}  on-device LLM engine — any GGUF (llama.cpp; stub unless enabled)
  renderhost.{hpp,cpp}     LaTeX/ECharts via one offscreen page + the disk cache; markdown -> rich text
  render/render.html       that page (MathJax + ECharts); its libraries come from harmony/
  qml/
    Main.qml               top bar + login/chat loader + shared dialogs
    LoginPage.qml  ChatPage.qml  MessageDelegate.qml
    ReplyBody.qml          a reply, split into prose / figure / chart segments
    ReplySegment.qml       one such segment (rich text, or an Image over its fallback)
    ToolCard.qml           one tool invocation: "Calling <tool>…", arguments, result
    HistoryDrawer.qml      the app's only menu: history + health + app settings + account
    CostDialog.qml  BackendDialog.qml  BleDialog.qml  LanguageDialog.qml  FontDialog.qml
    OnDeviceDialog.qml     the on-device model manager (catalog + your own; also picks one)
    ConfirmDialog.qml
    Theme.qml              singleton: Material 3 colour scheme (mirrors config.js)
    I18n.qml               singleton: reactive i18n facade
    strings.js             UI string table (verbatim from htdoc/src/i18n.js; regenerate if it changes)
    slash.js               slash commands (port of htdoc/src/slash.js)
    markdown.js            fence splitting + the $…$-vs-currency rule (port of android's)
  tests/                   ctest: the BLE decoder, and markdown.js via QJSEngine
```
