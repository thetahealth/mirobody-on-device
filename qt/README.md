# Mirobody Qt desktop client

A standalone **Qt Quick (QML)** desktop client for a running mirobody server —
the native counterpart of the web client in [`res/htdoc`](../res/htdoc). It
speaks the same HTTP + Server-Sent-Events API and links **nothing** from
`mirobody_core`; it is a pure API client built on Qt's own networking stack.

## What it does

Full parity with the web client's main flow:

- **Email one-time-code sign-in** — `POST /email/login` then `POST /email/verify`
  (six-digit code, resend cooldown). The token is persisted with `QSettings`.
- **Streaming chat** — `POST /api/chat` over SSE, both modes the web client
  supports:
  - *Agent mode* (a provider `"Agent/provider"` is selected): streams
    `{type:"reply"|"thinking"|"costStatistics"|"error"}` events, with a live
    "thinking" block and a cost/usage footer.
  - *Proxy mode* (no provider): sends `{model, messages, stream}` and reads the
    OpenAI-style `choices[0].delta.content` chunks, including the resumable
    `{session_id}` control chunk.
- **Provider picker** — `POST /api/providers`, restored/persisted selection.
- **On-device private LLM** *(optional)* — a synthetic **"Gemma 4 · On-device"**
  provider runs the model fully locally (no server, works offline), emitting the
  same reply stream so the chat UI is unchanged. The ~2.5 GB `.litertlm` model is
  downloaded on demand (`ModelDownloader`). The engine (`LocalLmEngine`, Gemma 4
  via LiteRT-LM C++) is compiled only with `-DMIROBODY_ONDEVICE_LLM=ON`; without
  it the option still appears but reports the feature isn't built in.
- **Direct Bluetooth (BLE GATT) health sensors** — scan for a standard-profile BLE
  sensor (HR strap, blood-pressure cuff, thermometer), connect, and stream its
  readings straight into the server's FHIR store. This is the desktop counterpart of
  the mobile HealthKit / Health Connect readers: `BleHealth` (built on Qt Bluetooth's
  `QLowEnergyController`) decodes each GATT / IEEE-11073 measurement, maps it to a FHIR
  R4 `Observation`, and `POST`s it to `/fhir/Observation` (the same ingestion path the
  phones use — the server gains no BLE code). Only standard-profile devices are
  readable; consumer watches/rings use proprietary/encrypted GATT and stay on the cloud
  vendor clients. See [`src/health/README.md`](../src/health/README.md) → *Direct
  Bluetooth devices*. Supported services: Heart Rate `0x180D`, Blood Pressure `0x1810`,
  Health Thermometer `0x1809` (adding one is a row in the decode dispatch in
  [`blehealth.cpp`](blehealth.cpp)). Reached from **⚙ → Bluetooth devices** when signed in.
- **Local conversation persistence** — the running conversation is mirrored to a
  per-user JSON file under `QStandardPaths::AppDataLocation`
  (`conversation-<userid>.json`, keyed by the JWT `sub`), the desktop stand-in
  for the web client's IndexedDB store.
- **History drawer** — `GET /api/history` (list) and `POST /api/history/delete`.
- **Settings** — backend URL (with presets), language (all ten the app offers;
  the choice rides on each agent request and localises the UI), font size, about.
- **Markdown** assistant replies, RTL mirroring for Arabic/Hebrew.

### Intentionally omitted

The web client's **social sign-ins** (Google/Apple/WeChat/GitHub/X) rely on
browser SDKs and popup/redirect flows that have no native Qt equivalent without
embedding a web engine and per-provider OAuth. Email login reaches every
account, so the native client ships email-only. Math (KaTeX) rendering inside
replies is likewise not reproduced — Markdown is rendered, math is shown as
source.

## Layout

```
qt/
  CMakeLists.txt        Qt6 Quick app + QML module (Mirobody)
  main.cpp              engine bootstrap; exposes AppController as `app`
  apiclient.{hpp,cpp}   HTTP envelope + SSE streaming (the net.js analogue) + raw FHIR POST
  chatmodel.{hpp,cpp}   QAbstractListModel of the transcript
  blehealth.{hpp,cpp}   direct BLE GATT sensor scan/connect -> FHIR Observation ingestion
  appcontroller.{hpp,cpp}  settings, login, providers, streaming, persistence
  modeldownloader.{hpp,cpp}  on-device model download (Hugging Face) + progress
  locallmengine.{hpp,cpp}  on-device Gemma 4 engine (LiteRT-LM; stub unless enabled)
  qml/
    Main.qml            top bar + login/chat loader + shared dialogs
    LoginPage.qml  ChatPage.qml  MessageDelegate.qml
    SettingsMenu.qml  HistoryDrawer.qml
    CostDialog.qml  BackendDialog.qml  BleDialog.qml  LanguageDialog.qml  FontDialog.qml
    AboutDialog.qml  ConfirmDialog.qml
    Theme.qml           singleton: the Material 3 colour scheme (config.js)
    I18n.qml            singleton: reactive i18n facade
    strings.js          UI string table (auto-derived from htdoc/src/i18n.js)
```

`strings.js` is copied verbatim from the web client's `i18n.js` `STRINGS` table
so the two surfaces read identically; regenerate it if `i18n.js` changes.

## Build

It is **off by default** and built only when `MIROBODY_BUILD_QT=ON` (and never
on mobile). It needs **Qt 6.5+** (Core, Gui, Qml, Quick, QuickControls2,
Network, **Bluetooth**).

**Toolchain:** on **Windows use MSVC** — the `msvc2022_64` Qt kit built with
Visual Studio's `cl.exe` (run inside `vcvarsall.bat amd64`), matching the rest
of the Windows build (`build.cmd`) and, importantly, LiteRT-LM's own MSVC
Windows build so the on-device engine can link. On **Linux/macOS use the system
GCC/Clang** (Qt's GCC/Clang kit). Do **not** mix compilers (a MinGW Qt build
cannot link MSVC libraries and vice-versa).

**Easiest — the wrapper scripts** (repo root). Because this target pulls in no
`mirobody_core` dependencies, `build-qt.cmd` / `build-qt.sh` build it standalone
(they generate the tiny `add_subdirectory(qt)` wrapper for you, since `qt/` has
no `project()` of its own):

```cmd
:: Windows (MSVC via vcvars; needs the msvc2022_64 kit)
build-qt.cmd            :: -> build-qt\app\mirobody_qt.exe
build-qt.cmd deploy     :: also runs windeployqt so the exe is double-clickable
```
```sh
# Linux / macOS (system GCC/Clang)
./build-qt.sh           # -> build-qt/app/mirobody_qt
```

Override the kit / toolchain paths with `QT_PREFIX`, `VS_DIR`, `NINJA` (Windows)
or `QT_PREFIX` (Linux/macOS). The generated `Mirobody` QML module lives on disk
next to the exe (in `build-qt/app/`), so keep the exe there — don't relocate it
away from its module directory.

**Or from the top-level build:**

```sh
cmake -B build-qt -S . -DMIROBODY_BUILD_QT=ON \
      -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/<compiler>   # msvc2022_64 on Windows
cmake --build build-qt --target mirobody_qt
```

The `MIROBODY_QT_VERSION` cache variable sets the version shown in the About
dialog (defaults to `dev`).

**On-device LLM (optional, experimental).** Without the flag below the client
still builds and links — the on-device provider is present but inert.

- **Simplest — just try Gemma locally first:** LiteRT-LM ships a prebuilt CLI,
  so no build is required — `uv tool install litert-lm` then
  `litert-lm run --from-huggingface-repo=litert-community/gemma-4-E2B-it-litert-lm gemma-4-E2B-it.litertlm --backend=cpu --prompt="hi"`.
  It also runs an OpenAI-compatible local server (`litert-lm serve`).
- **Embed it in this client:** add `-DMIROBODY_ONDEVICE_LLM=ON
  -DLITERT_LM_SDK_DIR=<dir with include/ and lib/>`. LiteRT-LM now has an
  official **CMake** build (`docs/getting-started/cmake.md`, verified on
  Ubuntu + GCC) and ships prebuilt per-platform runtime libraries — it is no
  longer Bazel-only. Build/obtain the SDK **with the same toolchain as this
  client** (MSVC on Windows, GCC/Clang on Linux/macOS) so it links. The C++
  glue in `locallmengine.cpp` tracks a specific LiteRT-LM API and may need
  updating against the SDK you build.

## Run

Launch `mirobody_qt`, open **⚙ → Backend**, and point it at a mirobody server
(the default is `http://127.0.0.1:8080`, matching `config.yml`'s `HTTP_PORT`).
Sign in with an email + code — once demo login is enabled on that backend
(uncomment `EMAIL_PREDEFINE_CODES` in its `config.yml`, off by default), e.g.
`demo1@mirobody.ai` / `777777`.

**Bluetooth permission.** BLE scanning is gated by the OS. On macOS the app must
carry an `NSBluetoothAlwaysUsageDescription` string (Info.plist) and `BleHealth`
requests the runtime `QBluetoothPermission` on first scan; Windows/Linux need
Bluetooth enabled but no per-app grant. `MACOSX_BUNDLE` is already set on the target.
