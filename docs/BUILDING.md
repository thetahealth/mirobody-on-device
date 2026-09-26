# Building mirobody

Full build instructions for every target. For the short desktop path,
see [Try the core on desktop](../README.md#try-the-core-on-desktop) in the README.

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
| libwebsockets            | The optional loopback HTTP front door (development build; off in the mobile profile) | MIT |
| libcurl (OpenSSL backend)| Upstream HTTPS requests                                      | MIT/X (curl) |
| OpenSSL                  | TLS for libwebsockets and libcurl                           | Apache-2.0 |
| RapidJSON                | JSON parsing / serialization                                 | MIT |
| yaml-cpp                 | Config file parsing                                          | MIT |
| SQLite                   | The record: one database file                                | Public domain |
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

### Database

SQLite is the only backend, and it is linked into every build: the phone keeps
its record in one file, and the development build uses the same schema
([res/sql/sqlite](../res/sql/sqlite)) so what the tests cover is what ships. The
server-side databases (Postgres and friends) live in the
[main mirobody repo](https://github.com/thetahealth/mirobody).

### vcpkg manifest *(Windows only)*

[`vcpkg.json`](../vcpkg.json) lists the dependencies a fresh Windows checkout
pulls. `tesseract` is parked in the `$dependencies` array (vcpkg ignores
`$`-prefixed fields) because OCR is off by default. If you'd rather not use vcpkg,
install the libraries directly and point cmake at them via
`-DCMAKE_PREFIX_PATH=<install-root>`.

## Toolchain

The shared core requires **C++17**. CMake sets `CMAKE_CXX_STANDARD 17`, marks it
required, disables compiler extensions, and exports `cxx_std_17` from
`mirobody_core`. `src/platform/log.hpp` contains a compile-time assertion so a
host that accidentally downgrades the target fails at the boundary instead of
failing later on a standard-library symbol.

C++17 is the portability floor chosen for the Android NDK and Apple toolchains;
the HarmonyOS native build must also pass before we treat that host as verified.
C++20 is not a project requirement: three hosts consume the native library on
independent release schedules, and the core currently needs no C++20 feature.

| Build path | Configuration in this repository | Verification |
|---|---|---|
| Linux desktop | Ubuntu 24.04 CI | Core, tests and mobile CMake profile |
| macOS desktop | macOS 15 CI | Core, tests and mobile CMake profile |
| Windows desktop | Visual Studio 2019 or newer in `build.cmd` | Not in current CI |
| Android host | NDK `27.0.12077973` in `android/app/build.gradle.kts` | App/native build not in current CI |
| iOS host | iOS 16 deployment target in `ios/project.yml` | App/XCFramework build not in current CI |
| HarmonyOS host | DevEco Studio 6.0 / native SDK API 21 per `harmony/README.md` | App/native build not in current CI |

Use the standard library directly. The old `src/compat/cxx11.hpp` optional shim
has been removed; `std::optional` and `std::nullopt` are part of the core API
implementation now. The public host boundary remains C, so this change does not
expose C++ types to Kotlin, Swift or ArkTS. Rebuild any existing native binary
or XCFramework because the internal C++ ABI has changed.

## Building - desktop

### Windows (MSVC + CMake + Ninja + vcpkg)

#### 1. Install Visual Studio

**Visual Studio 2019 or newer** for the C++17 core. Community edition is fine - download from
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

The target **arch is a command-line token** to `build.cmd` (see step 4),
not environment variables. The variables below only point `build.cmd` at your
toolchain; all are optional. Set them once in your user environment (PowerShell
`[Environment]::SetEnvironmentVariable`, `setx` from cmd, or *System Properties
-> Environment Variables*) and every new shell picks them up:

| Variable     | Required? | Points at                                        | Example                                                                                  |
| ------------ | --------- | ------------------------------------------------ | ---------------------------------------------------------------------------------------- |
| `VS_DIR`     | optional  | Visual Studio install root (contains `VC\...`)     | `%ProgramFiles%\Microsoft Visual Studio\18\Community` - script default if unset.       |
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

The optional lanes (Android, HarmonyOS, the on-device LLM, the terminology
tooling) take a few more, listed together under
[Environment variables](#environment-variables) below.

#### 4. Build

```cmd
build.cmd              :: host arch development build   -> build\
build.cmd mobile       :: the HarmonyOS profile          -> build-mobile\
build.cmd arm64        :: cross-compile                  -> build-arm64\
build.cmd clean        :: reconfigure from scratch
build.cmd -h           :: full token list
```

Output: `build\mirobody.exe` (the development server), `build\tests\mirobody_tests.exe`,
and the debug CLIs.

### Linux / WSL / macOS

Install the dependencies from your system package manager - vcpkg is only used
on Windows. CMake's `find_package` picks them up directly.

```sh
# Debian / Ubuntu / WSL: core dependencies (always needed)
sudo apt install build-essential cmake ninja-build pkg-config \
                 libwebsockets-dev libcurl4-openssl-dev libssl-dev \
                 rapidjson-dev libyaml-cpp-dev libsqlite3-dev \
                 libjpeg-dev libpng-dev libtiff-dev libwebp-dev catch2

# Fedora / RHEL: core dependencies
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config \
                 libwebsockets-devel libcurl-devel openssl-devel \
                 rapidjson-devel yaml-cpp-devel sqlite-devel \
                 libjpeg-turbo-devel libpng-devel libtiff-devel libwebp-devel catch2-devel

# macOS (Homebrew): core dependencies
brew install cmake ninja pkg-config libwebsockets curl openssl@3 \
             rapidjson yaml-cpp sqlite \
             jpeg-turbo libpng libtiff webp catch2
```

> **libwebsockets and response compression.** The packaged `libwebsockets`
> (apt / dnf / Homebrew) is built with `LWS_WITH_HTTP_STREAM_COMPRESSION` off, so
> the development server answers uncompressed. It only ever talks to loopback.

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

Then build via the wrapper:

```sh
./build.sh             # host arch development build   -> build/
./build.sh mobile      # the HarmonyOS profile          -> build-mobile/
./build.sh clean       # reconfigure from scratch
./build.sh -h          # full token list
```

On macOS the wrapper adds Homebrew's keg-only `jpeg-turbo` to `CMAKE_PREFIX_PATH`.
`build.sh` builds natively for the host arch.

Or invoke CMake directly if you prefer:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build/mirobody` (the development server), `build/tests/mirobody_tests`, and
the debug CLIs. Run the tests with `build/tests/mirobody_tests`.

### Runtime

```sh
./build/mirobody                        # from the repo root: config.example.yml, then ./config.yml, then env
./mirobody --help                       # also: -h, /?
```

`SIGINT` / `SIGTERM` (or Ctrl-C / Ctrl-Break on Windows) trigger a graceful
shutdown.

## Environment variables

The desktop build needs at most the four in
[Set environment variables](#3-set-environment-variables) above, and often none.
Everything below belongs to an *optional* lane — Android, HarmonyOS, the
on-device LLM, or the terminology tooling — and tells
that lane where you installed its toolchain or put its reference data.

**No script hardcodes a drive letter.** Each discovers what it can from the
standard install locations, and when it can't, it fails naming the variable to
set. So set only the lanes you actually build, and only when the default is
wrong for your machine.

| Variable | Lane | Points at | Default if unset |
| -------- | ---- | --------- | ---------------- |
| `ANDROID_HOME` | Android | Android SDK root | `%LOCALAPPDATA%\Android\Sdk` |
| `NDK_HOME` / `ANDROID_NDK_HOME` | Android | NDK for cross-compiling the deps (`.cmd` / `.sh` respectively) | the pinned NDK under `$ANDROID_HOME\ndk`, else the newest installed there |
| `MIROBODY_NDK_PATH` | Android | space-free NDK mirror for Gradle | the mirror `build-app.cmd` makes (Windows only; `build-app.sh` passes `ANDROID_NDK_HOME` straight through) |
| `GRADLE_BIN` | Android | `gradle` launcher (the one on PATH is usually too old) | the pinned Gradle under `%USERPROFILE%\.gradle\` |
| `DEVECO_HOME` | HarmonyOS | DevEco Studio install root | `%ProgramFiles%\Huawei\DevEco Studio` |
| `OHOS_SDK_ROOT` | HarmonyOS | the SDK dir *containing* `native\`; skips the DevEco probe | derived from `DEVECO_HOME` |
| `LLAMA_SRC` | on-device LLM | llama.cpp checkout — one clone serves Android, iOS and HarmonyOS | `llama.cpp` beside the repo. `harmony\build-llama` never clones it — it prints the `git clone` line and stops, so the checkout the device numbers were measured against stays pinned |
| `LLAMA_SDK_DIR` | on-device LLM (HarmonyOS) | where to assemble the cross-built SDK. Moving it off the default means naming it with `-DLLAMA_CPP_DIR` in `harmony\entry\build-profile.json5`, which the default exists to avoid | `harmony\prebuilt\llama-sdk\<abi>` — under the same `prebuilt\` parent as the vcpkg deps, found there by the module's CMake |
| `MIROBODY_REF` | terminology | raw reference-data root for `indicator build-lexicon` / `build-units` | **none** - pass `--ref`, or the command exits 2 |

Non-path knobs live where they apply rather than here: `MIROBODY_CONFIG` under
[Runtime](#runtime) and `MIROBODY_VCPKG_CACHE` under
[Set environment variables](#3-set-environment-variables).

Each lane's own README repeats the two or three variables it needs, in context:
[`android/`](../android/README.md), [`harmony/`](../harmony/README.md),
[`src/indicator/`](../src/indicator/README.md).

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

### On-device LLM (llama.cpp)

[`ios/build-llama.sh`](../ios/build-llama.sh) cross-compiles llama.cpp into
`ios/prebuilt/llama-sdk/<sdk>-arm64/{include,lib}` - static archives, since iOS
cannot dlopen code the app wrote and has nowhere to put a loose `.so`. Add
`-DMIROBODY_ONDEVICE_LLM=ON -DLLAMA_CPP_DIR=<that dir>` to the matching configure
above and rebuild the xcframework; the script prints the exact flags. Link the
archives into the app target, plus `Metal.framework` and `Accelerate.framework`
on the device slice.

Unlike Android, nothing finds this by itself - the iOS core is configured by
hand, so the flags are yours to pass. Omit them and `src/llm/local.cpp` compiles
its stub, `mirobody_llm_available()` returns 0, and `LlamaCppEngine` reports that
the engine is not built in.

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

## Embedding - the C ABI

[src/mirobody.h](../src/mirobody.h) is the embedding surface: a plain `extern "C"`
API that the platform bridges wrap (Android JNI, the iOS static library, the
HarmonyOS NAPI module). The development build also produces it as a shared
library, `libmirobody` (`mirobody.dll` / `libmirobody.so` / `.dylib`), which is
handy for exercising the ABI from a test harness. The desktop FFI bindings (Go,
C#, Node, Rust, Java) and the Python wheel were removed when this repo narrowed to
the phone; they are preserved at the `v2-full-2026-08` tag.
