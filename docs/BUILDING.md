# Building mirobody

Full build instructions for every target. For the condensed one-liner per
platform, see the [Quick start](../README.md#quick-start) in the main README.

## Dependencies

How each platform sources its native libraries:

| Platform           | Source                                                                                  |
| ------------------ | --------------------------------------------------------------------------------------- |
| Windows            | vcpkg manifest ([`vcpkg.json`](../vcpkg.json)) - see "Building - Windows" below.           |
| Linux / WSL / macOS | System package manager (`apt` / `dnf` / `brew`) - see "Building - Linux / WSL / macOS". |
| Android / iOS      | Prebuilt sysroots under `android/prebuilt/<ABI>/` and `ios/prebuilt/<sdk>-<arch>/`.     |

Library list:

| Package                  | Purpose                                                      | License |
| ------------------------ | ------------------------------------------------------------ | ------- |
| libwebsockets            | HTTP + WebSocket server / client                             | MIT |
| libcurl (OpenSSL backend)| Upstream HTTPS requests                                      | MIT/X (curl) |
| OpenSSL                  | TLS for libwebsockets and libcurl                           | Apache-2.0 |
| RapidJSON                | JSON parsing / serialization                                 | MIT |
| yaml-cpp                 | Config file parsing                                          | MIT |
| hiredis                  | Redis protocol client (remote KV / cache backend)            | BSD-3-Clause |
| libjpeg / libpng / libtiff / libwebp | Image decode / encode (image transcoder)        | BSD-3 / zlib / IJG (all permissive) |
| xlnt                     | `.xlsx` reader for the document transcoder (auto-detected; required on Windows via vcpkg, optional elsewhere) | MIT |
| PDFium *(optional)*      | PDF text extraction + page rasterization; enable with `-DMIROBODY_ENABLE_PDF=ON` (prebuilt binary, e.g. bblanchon/pdfium-binaries) | BSD-3-Clause (binaries wrapped MIT) |
| Tesseract *(optional)*   | OCR for scanned PDF pages; enable with `-DMIROBODY_ENABLE_OCR=ON` (implies `-DMIROBODY_ENABLE_PDF=ON`) | Apache-2.0 (+ Leptonica BSD-2-Clause) |
| libxls *(optional)*      | Legacy binary `.xls` reader; enable with `-DMIROBODY_ENABLE_XLS=ON` (no vcpkg port — vendored) | BSD-2-Clause |
| OpenBLAS *(optional)*    | Sets `MIROBODY_HAS_BLAS=1` when `BLAS` is found              | BSD-3-Clause |

All of the above are permissive (no copyleft), so they impose no source-disclosure
obligation on the proprietary mobile hosts the core embeds into. The document-transcoder
deps (xlnt, PDFium, Tesseract/Leptonica, libxls) were license-verified against their
upstream `LICENSE` files; the optional libraries' transitive dependencies should be
re-checked at the point they are actually built (e.g. via each vcpkg port's installed
`copyright` file). Tesseract's `*.traineddata` language files bundled under `res/` are a
separate artifact (Apache-2.0, from `tessdata_fast` / `tessdata_best`).

### Database client libraries

Exactly one of these is linked into `mirobody_core`, picked by
`MIROBODY_DATABASE_BACKEND`. SQLite is the mobile default; `POSTGRESQL`
(the libpq client with the modern `res/sql/pg` schema) is the desktop
default - install only the one(s) you actually plan to build against.

| Package                       | Direct download                                                          |
| ----------------------------- | ------------------------------------------------------------------------ |
| SQLite amalgamation           | https://sqlite.org/download.html                                         |
| PostgreSQL (ships `libpq`)    | https://www.postgresql.org/download/                                     |
| Oracle MySQL Connector/C      | https://dev.mysql.com/downloads/connector/c/                             |
| MariaDB Connector/C (LGPL)    | https://mariadb.com/downloads/connectors/connectors-data-access/c-connector |
| DuckDB C/C++ library          | https://duckdb.org/docs/installation/                                    |

### vcpkg manifest *(Windows only)*

[`vcpkg.json`](../vcpkg.json) lists every backend so a fresh Windows checkout
pulls them all; the ones you don't select get compiled by vcpkg and ignored
at link time. To trim install time, drop the unwanted entries from
`vcpkg.json` before configuring - or move them into the parked `$dependencies`
array (vcpkg ignores `$`-prefixed fields), which is how `duckdb` and
`libmysql` are currently kept out of the default build.

If you'd rather not use vcpkg on Windows either, install the client library
directly and point cmake at it via `-DCMAKE_PREFIX_PATH=<install-root>`. The
CMake `find_package` calls currently use vcpkg-style target names, so the
MySQL and DuckDB branches in `CMakeLists.txt` may need a small tweak when
consuming a non-vcpkg install.

## Toolchain

C++11 is the floor. Set via `CMAKE_CXX_STANDARD 11` / `_REQUIRED ON` /
`_EXTENSIONS OFF`; `mirobody_core` exposes `cxx_std_11` as a public
`target_compile_features` so every downstream target inherits the
requirement, and a `static_assert` in
[src/platform/log.hpp](../src/platform/log.hpp) catches any consumer that
slips through. MSVC builds add `/W4 /utf-8 /permissive- /Zc:__cplusplus`.

| Compiler    | Minimum                                              |
| ----------- | ---------------------------------------------------- |
| MSVC        | Visual Studio 2015 (2017 / 2019 / 2022 / Preview 18) |
| GCC         | 4.8.1 (Linux / WSL)                                  |
| Clang       | 3.3 (Linux / WSL)                                    |
| Apple Clang | Xcode 5 (iOS / macOS)                                |
| Android NDK | r14                                                  |

### C++11

The code was retreated to C++11 to widen the
set of host environments mirobody can embed into. Features that are post-C++11
are shimmed in [src/compat/cxx11.hpp](../src/compat/cxx11.hpp):

- `mirobody::optional<T>` / `mirobody::nullopt` - minimal drop-in for
  `std::optional` using `std::aligned_storage` under the hood.

(The codebase deals in `const std::string&` / `const char*` / `(const void*,
size_t)` directly rather than a `string_view` shim.)

## Building - desktop

### Windows (MSVC + CMake + Ninja + vcpkg)

#### 1. Install Visual Studio

**Visual Studio 2017 or newer** (2019 / 2022 / Preview 18 all work; 2015 also
compiles but Ninja didn't ship with it, so you'd have to install Ninja
separately). Community edition is fine - download from
[visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/).

In the installer, under *Workloads*, tick **Desktop development with C++**.
Then under *Individual components*, also tick **C++ CMake tools for Windows** -
that's the package that drops `ninja.exe` into
`<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\`, which is exactly
where `build.cmd` looks for it. No separate Ninja install is needed.

#### 2. Get vcpkg *(optional)*

Visual Studio's "C++ CMake tools for Windows" component ships its own vcpkg at
`%VS_DIR%\VC\vcpkg`. `vcvarsall.bat` exports `VCPKG_ROOT` pointing at it, and
`build.cmd` will use that by default - **you can skip this step.**

You might still want a standalone clone if you'd like:

- **Independent updates.** Refresh `vcpkg` (and the `builtin-baseline` it
  pins) without waiting for a Visual Studio update - handy when you need a
  port fix that hasn't landed in VS yet.
- **Stable cache across VS upgrades.** A VS update may reset the bundled
  `VC\vcpkg\` directory, including any in-place state. A standalone clone
  isn't touched.
- **One vcpkg shared across machines / IDEs.** Same checkout works from
  CLion, Rider, CI, plain `cmake`, etc. - no need to keep them in sync with a
  specific VS install.

If you want one, clone and bootstrap anywhere on disk:

```cmd
git clone https://github.com/microsoft/vcpkg.git C:\Tools\vcpkg
C:\Tools\vcpkg\bootstrap-vcpkg.bat
```

Pick any path you like in place of `C:\Tools\vcpkg`. Then set `VCPKG_ROOT` in
step 3 below so the script picks your checkout over VS's bundled one.

**Slow GitHub?** The clone above pulls vcpkg's full history (~300k objects). On
a throttled connection, a shallow clone from a mirror is far quicker. Because
`--depth 1` only contains the tip commit (not the `builtin-baseline` pinned in
`vcpkg.json`), re-pin the baseline to that commit afterwards - this repo has no
version constraints, so the single commit is enough:

```cmd
git clone --depth 1 https://gitclone.com/github.com/microsoft/vcpkg.git C:\Tools\vcpkg
C:\Tools\vcpkg\bootstrap-vcpkg.bat
cd /d {your_project_directory}
C:\Tools\vcpkg\vcpkg.exe x-update-baseline
```

#### 3. Set environment variables

Target **arch and backend are command-line tokens** to `build.cmd` (see step 4),
not environment variables. The variables below only point `build.cmd` at your
toolchain; all are optional. Set them once in your user environment (PowerShell
`[Environment]::SetEnvironmentVariable`, `setx` from cmd, or *System Properties
-> Environment Variables*) and every new shell picks them up:

| Variable     | Required? | Points at                                        | Example                                                                                  |
| ------------ | --------- | ------------------------------------------------ | ---------------------------------------------------------------------------------------- |
| `VS_DIR`     | optional  | Visual Studio install root (contains `VC\...`)     | `C:\Program Files\Microsoft Visual Studio\18\Community` - script default if unset.       |
| `VCPKG_ROOT` | optional  | vcpkg checkout (contains `scripts\buildsystems`) | `C:\Tools\vcpkg` - only set if you went with step 2. Otherwise vcvars points at VS-bundled.    |
| `NINJA`      | optional  | `ninja.exe` path                                 | `C:\Tools\ninja\ninja.exe` - defaults to the copy under `%VS_DIR%\Common7\IDE\...\Ninja\`. |
| `MIROBODY_VCPKG_CACHE` | optional | shared vcpkg binary cache dir          | `\\nas\team\vcpkg-cache` - reused across machines and build dirs (created if missing); lets a fresh build pull prebuilt packages instead of compiling from source. |

```cmd
:: Only set the ones whose defaults don't match your install.
setx VS_DIR "C:\Program Files\Microsoft Visual Studio\18\Community"
setx VCPKG_ROOT C:\Tools\vcpkg                :: only if you cloned your own vcpkg
setx NINJA C:\Tools\ninja\ninja.exe           :: only if you installed Ninja separately
setx MIROBODY_VCPKG_CACHE \\nas\vcpkg-cache   :: only to share a vcpkg binary cache
```

Override these via the environment rather than editing the script. To switch
*back* to VS-bundled vcpkg after `setx`-ing `VCPKG_ROOT`, remove the variable
(an empty `setx` only blanks it; this fully unsets it):

```powershell
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", $null, "User")
```

#### 4. Build

```cmd
build.cmd              :: host arch + POSTGRESQL        -> build\
build.cmd legacy       :: POSTGRESQL_LEGACY             -> build_legacy\
build.cmd sqlite       :: SQLITE                        -> build_sqlite\
build.cmd legacy clean :: reconfigure that dir from scratch
build.cmd -h           :: full token list (arch / backend / clean)
```

Backend token: `pg` / `postgresql`, `legacy` / `pg_legacy`, `mysql`, `sqlite`,
`duckdb`, `ck` / `clickhouse` (omit for the `POSTGRESQL` default). Arch
token: `amd64` (default, the host) or `arm64` / `x86` to cross-compile. Each
arch+backend combo lives in its own `build[_<arch>][_<backend>]` directory and
they all share one `vcpkg_installed`, so you can keep several configured at once.

Output: `build\mirobody.exe` (or `build_legacy\mirobody.exe`, … per backend).

### Linux / WSL / macOS

Install the dependencies from your system package manager - vcpkg is only used
on Windows. CMake's `find_package` picks them up directly.

```sh
# Debian / Ubuntu / WSL: core dependencies (always needed)
sudo apt install build-essential cmake ninja-build pkg-config \
                 libwebsockets-dev libcurl4-openssl-dev libssl-dev \
                 rapidjson-dev libyaml-cpp-dev libhiredis-dev \
                 libjpeg-dev libpng-dev libtiff-dev libwebp-dev

# Fedora / RHEL: core dependencies
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config \
                 libwebsockets-devel libcurl-devel openssl-devel \
                 rapidjson-devel yaml-cpp-devel hiredis-devel \
                 libjpeg-turbo-devel libpng-devel libtiff-devel libwebp-devel

# macOS (Homebrew): core dependencies
brew install cmake ninja pkg-config libwebsockets curl openssl@3 \
             rapidjson yaml-cpp hiredis \
             jpeg-turbo libpng libtiff webp
```

The image codecs above also cover the document transcoder's only always-on
external dep path (CSV is built-in, no library). The document transcoder's other
formats need extra libraries, none of which break the build when absent - the
matching format just reports "not built" at runtime:

```sh
# .xlsx (xlnt): NOT packaged for Debian/Ubuntu/Fedora/Homebrew - build from source
# (https://github.com/xlnt-community/xlnt). CMake auto-detects it; without it the
# build still succeeds and .xlsx is disabled (CSV/PDF unaffected).

# OCR for scanned PDFs (-DMIROBODY_ENABLE_OCR=ON, implies -DMIROBODY_ENABLE_PDF=ON):
sudo apt install tesseract-ocr libtesseract-dev libleptonica-dev tesseract-ocr-eng  # Debian / Ubuntu / WSL
sudo dnf install tesseract tesseract-devel leptonica-devel tesseract-langpack-eng   # Fedora / RHEL
brew install tesseract                                                              # macOS (bundles leptonica + eng data)

# PDF (-DMIROBODY_ENABLE_PDF=ON): PDFium is not packaged anywhere - download a
# prebuilt no-V8 release (https://github.com/bblanchon/pdfium-binaries) and point
# -DCMAKE_PREFIX_PATH at it.
# Legacy .xls (-DMIROBODY_ENABLE_XLS=ON): libxls has no package - vendor it.
```

Then install **one** database client library matching the backend you
plan to build against (PostgreSQL is the desktop default; SQLite isn't a
desktop option; see the [Database backends](../src/database/README.md) doc for the
full list):

```sh
# PostgreSQL
sudo apt install libpq-dev                        # Debian / Ubuntu / WSL
sudo dnf install libpq-devel                      # Fedora / RHEL
brew install libpq                                # macOS

# MySQL (or libmysqlclient-dev / mysql-devel / mysql-client for Oracle MySQL)
sudo apt install libmariadb-dev
sudo dnf install mariadb-connector-c-devel
brew install mariadb-connector-c

# DuckDB: download a release from https://duckdb.org/docs/installation/
# ClickHouse: stub backend today; no client library to install yet
```

Then build via the wrapper -- the backend is a command-line token (no env var),
passed in any order with `clean`:

```sh
./build.sh             # host arch + POSTGRESQL        -> build/
./build.sh legacy      # POSTGRESQL_LEGACY             -> build_legacy/
./build.sh sqlite      # SQLITE                        -> build_sqlite/
./build.sh legacy clean # reconfigure that dir from scratch
./build.sh -h          # full token list
```

Tokens: `pg` / `postgresql`, `legacy` / `pg_legacy`, `mysql`, `sqlite`, `duckdb`,
`ck` / `clickhouse` (omit for the `POSTGRESQL` default). Each backend
builds into its own `build[_<backend>]` directory, so several can coexist;
`build.sh` builds natively for the host arch.

Or invoke CMake directly if you prefer:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMIROBODY_DATABASE_BACKEND=POSTGRESQL
cmake --build build
```

Output: `build/mirobody` (or `build_legacy/mirobody`, … per backend).

### Runtime

```sh
./mirobody                              # reads ./config.yml by default; override with --config <path> or MIROBODY_CONFIG
./mirobody --help                       # also: -h, /?
```

`SIGINT` / `SIGTERM` (or Ctrl-C / Ctrl-Break on Windows) trigger a graceful
shutdown.

## Building - Android

The Android project lives under [android/](../android/) and pulls the same
`CMakeLists.txt` via `externalNativeBuild`. It builds the `mirobody` target as a
shared library (`libmirobody.so`) loaded by the Kotlin host app over JNI.

Today the JNI entry point just starts the embedded HTTP + WebSocket server on
loopback and the Kotlin app talks to it over `127.0.0.1`. The plan is to grow
`platform/android_jni.cpp` to call directly into `mirobody_core` (chat,
realtime, etc.) so the Kotlin side never opens a socket - same direction as
iOS below.

Currently the build only targets `arm64-v8a`. The native dependencies
(libwebsockets, libcurl, OpenSSL, yaml-cpp, OpenBLAS) are expected as prebuilt
sysroots under `android/prebuilt/<ABI>/`; `CMAKE_PREFIX_PATH` is wired up in
[android/app/build.gradle.kts](../android/app/build.gradle.kts). Producing those
prebuilts is out of scope for this README.

```sh
cd android
./gradlew assembleRelease
```

## Building - iOS

The iOS target builds `libmirobody.a` (plus the transitive `libmirobody_core.a`)
exposing the C API in [src/mirobody.h](../src/mirobody.h). A Swift or Objective-C
host links those archives and the prebuilt third-party dependencies, then calls
`mirobody_start` / `mirobody_stop` directly.

Same prebuilt-sysroot situation as Android: libwebsockets, libcurl, OpenSSL,
yaml-cpp, RapidJSON (header-only) must be cross-compiled for iOS ahead of time.
Drop them under `ios/prebuilt/<sdk>-<arch>/` (e.g. `iphoneos-arm64/`,
`iphonesimulator-arm64/`) and point `CMAKE_PREFIX_PATH` at the matching path
per configure.

Use the standard iOS CMake toolchain (e.g.
[leetal/ios-cmake](https://github.com/leetal/ios-cmake)):

```sh
# device slice
cmake -S . -B build-ios-arm64 \
    -G Xcode \
    -DCMAKE_TOOLCHAIN_FILE=path/to/ios.toolchain.cmake \
    -DPLATFORM=OS64 \
    -DDEPLOYMENT_TARGET=15.0 \
    -DCMAKE_PREFIX_PATH="$(pwd)/ios/prebuilt/iphoneos-arm64"
cmake --build build-ios-arm64 --config Release

# simulator slice
cmake -S . -B build-ios-simarm64 \
    -G Xcode \
    -DCMAKE_TOOLCHAIN_FILE=path/to/ios.toolchain.cmake \
    -DPLATFORM=SIMULATORARM64 \
    -DDEPLOYMENT_TARGET=15.0 \
    -DCMAKE_PREFIX_PATH="$(pwd)/ios/prebuilt/iphonesimulator-arm64"
cmake --build build-ios-simarm64 --config Release
```

Bundle the slices into an xcframework that Xcode and SwiftPM can both consume:

```sh
xcodebuild -create-xcframework \
    -library build-ios-arm64/Release-iphoneos/libmirobody.a \
    -library build-ios-simarm64/Release-iphonesimulator/libmirobody.a \
    -output mirobody.xcframework
```

Only `arm64` slices are built - 32-bit and `x86_64`-simulator are intentionally
skipped (no supported iOS hardware needs them).

### Consuming from Swift

Add [src/mirobody.h](../src/mirobody.h) to your Xcode bridging header:

```objc
#include "mirobody.h"
```

Then from Swift, on a `127.0.0.1` loopback today:

```swift
let handle = mirobody_start("/path/to/config.yml", nil)
defer { mirobody_stop(handle) }

// The listen port and LLM keys come from the config (HTTP_PORT / OPENAI_API_KEY
// / GOOGLE_API_KEY); read the bound port back from the handle.
let port = mirobody_listen_port(handle)
print("mirobody on 127.0.0.1:\(port)")
```

Loopback only - the bridge force-rewrites `0.0.0.0` to `127.0.0.1` so the host
app doesn't trigger iOS's local-network privacy prompt. When the HTTP/WS surface
is later replaced with direct C/C++ entry points, extend `mirobody.h` with the
new functions and the iOS host calls them the same way.

### iOS host app

A full SwiftUI host app - the iOS counterpart to [`android/`](../android) - lives
under [`ios/`](../ios). It builds and runs as a pure client (no native artifacts
required), and embeds `mirobody.xcframework` via the C API above once you build it.
See [ios/README.md](../ios/README.md) for the `xcodegen generate` build steps,
embedding instructions, and the Google-sign-in setup.

## Building - Python wheel

`mirobody._mirobody` is a CPython extension (a pybind11 module wrapping
`mirobody::Server`) packaged into a `.whl` by scikit-build-core. It exposes a
`Server` class - start/stop the embedded server in-process from Python:

```python
import mirobody
with mirobody.Server(listen_port=8080, openai_api_key="sk-...") as srv:
    print("listening on", srv.listen_port())
```

Build it with [build-python.cmd](../build-python.cmd) (Windows) /
[build-python.sh](../build-python.sh) (Linux / macOS). The build uses the
`*-windows-static-md` vcpkg triplet so the native deps are linked into the
`.pyd` and the wheel is self-contained - `import mirobody` needs no loose DLLs.
Full details, gotchas, and `pip` invocations are in [python/README.md](../python/README.md).

## Embedding - C-ABI shared library (Java, Go, C#, Node, Rust)

Beyond the platform bridges (Android JNI, iOS static lib) and the Python wheel,
the same [src/mirobody.h](../src/mirobody.h) C API is built as a standalone shared
library, **`libmirobody`** (`mirobody.dll` / `libmirobody.so` / `.dylib`), that
any language with an FFI loads and drives:

```c
mirobody_server_t* mirobody_start(config_path, data_dir);   // keys + port from config
void mirobody_stop(mirobody_server_t*);
int  mirobody_is_running(mirobody_server_t*);
int  mirobody_listen_port(mirobody_server_t*);
const char* mirobody_get_providers(void);                                    // "Agent/model" pairs
int  mirobody_chat(provider, message, user_id, on_event, user_data);         // serverless LLM + MCP
```

Build it with [build-shared.cmd](../build-shared.cmd) / [build-shared.sh](../build-shared.sh).
On Windows it defaults to the fully-static `x64-windows-static` triplet (`/MT`),
so the artifact depends only on system DLLs - no vcpkg DLLs and, importantly, no
`vcruntime`/`ucrtbase`, which is what lets it load cleanly into a JVM (whose
bundled CRT otherwise shadows the system one). When `JAVA_HOME` is set it also
builds **`mirobody_jni`**, a JNI shim for the desktop JVM
([src/platform/jni_bridge.cpp](../src/platform/jni_bridge.cpp), the same
`ai.thetahealth.mirobody.NativeBridge` class as Android).

Runnable, verified bindings and the full writeup live under
[bindings/](../bindings/) - see [bindings/README.md](../bindings/README.md):

| Language  | Binding                            | Mechanism                         |
| --------- | ---------------------------------- | --------------------------------- |
| Go        | [bindings/go](../bindings/go)         | `syscall.NewLazyDLL` (POSIX: cgo) |
| C# / .NET | [bindings/csharp](../bindings/csharp) | P/Invoke (`[DllImport]`)          |
| Node.js   | [bindings/node](../bindings/node)     | koffi FFI                         |
| Rust      | [bindings/rust](../bindings/rust)     | `extern "C"` + `build.rs`         |
| Java      | [bindings/java](../bindings/java)     | JNI shim, or Panama FFM           |
