#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Cross-compile llama.cpp for iOS and assemble an SDK dir the core links against.
# The Apple counterpart of android/build-llama.sh and harmony/build-llama.sh --
# same output layout (include/ + lib/), same contract with the root CMakeLists.
#
#   ./build-llama.sh [device|simulator|both] [clean]     (tokens in any order)
#
#     device       iphoneos-arm64        (default)
#     simulator    iphonesimulator-arm64
#     both         one after the other
#     clean        discard the cached build tree and SDK first
#
# macOS ONLY -- it needs Xcode's SDKs. There is no .cmd twin for that reason.
#
# Output: ios/prebuilt/llama-sdk/<sdk>-arm64/{include,lib}, beside the third-party
# sysroots in ios/prebuilt/<sdk>-arch/ that docs/BUILDING.md already expects. The
# core does NOT find this by itself the way Android's Gradle build does -- the iOS
# core is configured by hand, so pass the flags this script prints at the end.
#
# STATIC ARCHIVES, and there is no shared option. Android ships llama.cpp as
# dlopen-able modules to pick CPU kernels at runtime; iOS cannot -- the platform
# forbids loading code the app wrote, and the app has nowhere to put a loose .so
# anyway. Which also means the -march question does not arise here: every device
# that can run this app is arm64 with a known feature floor, so the toolchain
# default is the whole answer rather than a compromise.
#
# METAL on device, off for the simulator. Apple's GPU is the only accelerator
# either engine can reach on iOS (LiteRT-LM's Swift package ships no GPU backend
# at all), so it is worth having -- but UNMEASURED by us. Decode is bound by
# memory bandwidth, CPU and GPU share one pool on Apple silicon, and on a
# Snapdragon 865 that made the GPU a wash. Treat it as a lane to measure, not a
# win to assume. GGML_METAL_EMBED_LIBRARY puts the shaders in the archive so
# there is no .metallib resource for the app to ship and locate at runtime.
#
# Env overrides:
#   LLAMA_SRC          llama.cpp checkout (default: llama.cpp beside the repo)
#   LLAMA_SDK_DIR      where to assemble the SDK (default: as above)
#   DEPLOYMENT_TARGET  minimum iOS (default 15.0, matching docs/BUILDING.md)
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
cd "$(dirname "$SCRIPT")"
SCRIPT_DIR="$(pwd)"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "[build-llama] macOS only -- this needs Xcode's iOS SDKs." >&2
    exit 1
fi

TARGETS="" ; CLEAN=""
for tok in "$@"; do
    case "$tok" in
        clean)      CLEAN=1 ;;
        device)     TARGETS="$TARGETS device" ;;
        simulator)  TARGETS="$TARGETS simulator" ;;
        both)       TARGETS="device simulator" ;;
        -h|--help)  sed -n "2,40p" "$SCRIPT" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)          echo "[build-llama] unknown token '$tok'" >&2; exit 2 ;;
    esac
done
TARGETS="${TARGETS:-device}"

DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET:-15.0}"

# --- llama.cpp checkout -------------------------------------------------------
# Beside the repo, matching every other build-llama script -- one checkout serves
# all of them. Not cloned for you: a pinned checkout is what makes two
# measurements comparable.
LLAMA_SRC="${LLAMA_SRC:-$(cd "$SCRIPT_DIR/.." && pwd)/../llama.cpp}"
if [ ! -f "$LLAMA_SRC/include/llama.h" ]; then
    echo "[build-llama] no llama.cpp at \"$LLAMA_SRC\"." >&2
    echo "[build-llama] clone it, or set LLAMA_SRC to an existing checkout:" >&2
    echo "[build-llama]   git clone --depth 1 https://github.com/ggml-org/llama.cpp \"$LLAMA_SRC\"" >&2
    exit 1
fi

command -v cmake >/dev/null 2>&1 || { echo "[build-llama] cmake not on PATH." >&2; exit 1; }
command -v xcodebuild >/dev/null 2>&1 || { echo "[build-llama] Xcode not installed." >&2; exit 1; }

build_slice() {
    local kind="$1" sysroot metal
    case "$kind" in
        device)    sysroot="iphoneos"        ; metal=ON  ;;
        simulator) sysroot="iphonesimulator" ; metal=OFF ;;
    esac
    local build="$SCRIPT_DIR/.llama-build/$sysroot-arm64"
    local sdk="${LLAMA_SDK_DIR:-$SCRIPT_DIR/prebuilt/llama-sdk/$sysroot-arm64}"

    if [ -n "$CLEAN" ]; then
        echo "[build-llama] cleaning $sysroot"
        rm -rf "$build" "$sdk"
    fi

    echo "[build-llama] $sysroot-arm64  metal=$metal  min=$DEPLOYMENT_TARGET"

    # CMake's OWN iOS support (CMAKE_SYSTEM_NAME=iOS), not the leetal/ios-cmake
    # toolchain docs/BUILDING.md uses for the core. Deliberate: that toolchain is a
    # separate download, and llama.cpp's build already understands the native
    # variables. The two produce the same arm64 slices against the same SDK, and
    # the core build is free to keep using what it uses.
    #
    # GGML_NATIVE=OFF because probing the build machine says nothing about a phone,
    # even when both are Apple silicon. LLAMA_CURL=OFF: the app downloads models
    # itself, and curl here would drag in a second, differently-configured copy of
    # a dependency the core already links.
    cmake -S "$LLAMA_SRC" -B "$build" -G Ninja \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_SYSROOT="$sysroot" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DGGML_METAL="$metal" \
      -DGGML_METAL_EMBED_LIBRARY="$metal" \
      -DGGML_ACCELERATE=ON \
      -DGGML_NATIVE=OFF \
      -DGGML_OPENMP=OFF \
      -DLLAMA_CURL=OFF \
      -DLLAMA_BUILD_TESTS=OFF \
      -DLLAMA_BUILD_EXAMPLES=OFF \
      -DLLAMA_BUILD_SERVER=OFF \
      -DLLAMA_BUILD_TOOLS=OFF

    # Only the libraries: upstream's CLI targets link impl libs the OFF switches
    # above remove, so asking for one would fail the link -- and we want the
    # library, not the CLI. ggml-metal is a separate archive when Metal is on.
    local targets="llama ggml ggml-cpu ggml-base"
    [ "$metal" = "ON" ] && targets="$targets ggml-metal"
    # shellcheck disable=SC2086
    cmake --build "$build" --target $targets

    echo "[build-llama] assembling $sdk"
    rm -rf "$sdk"
    mkdir -p "$sdk/include" "$sdk/lib"
    cp "$LLAMA_SRC/include/llama.h" "$sdk/include/"
    cp "$LLAMA_SRC"/ggml/include/*.h "$sdk/include/"
    # Wherever they landed: the layout under the build tree has moved between
    # llama.cpp releases, and hardcoding src/ vs ggml/src/ breaks on the next one.
    find "$build" -name '*.a' -exec cp {} "$sdk/lib/" \;

    echo "[build-llama] done: $sdk"
    SDKS="$SDKS $sdk"
}

SDKS=""
for t in $TARGETS; do build_slice "$t"; done

echo
echo "[build-llama] The iOS core is configured by hand (docs/BUILDING.md), so add"
echo "[build-llama] these to the matching cmake configure and rebuild the xcframework:"
for s in $SDKS; do
    echo "[build-llama]   -DMIROBODY_ONDEVICE_LLM=ON -DLLAMA_CPP_DIR=$s"
done
echo "[build-llama] Then link the archives from that lib/ into the app target, and"
echo "[build-llama] -- on the device slice -- Metal.framework and Accelerate.framework."
