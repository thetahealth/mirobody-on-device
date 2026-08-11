#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Cross-compile llama.cpp for Android into android/prebuilt/llama-sdk/<abi>/, which the
# app's native build picks up by itself (see app/build.gradle.kts). The macOS/Linux
# counterpart of build-llama.cmd -- same tokens, same flags, same output layout -- as
# build-prebuilt.sh is of build-prebuilt.cmd.
#
#   ./build-llama.sh [abi] [clean]             (tokens in any order)
#
#     abi      arm64-v8a (default) | x86_64
#     clean    discard the cached build tree and SDK first
#
# WHY NOT ONE -march: a cross build cannot probe the target -- CMake's feature tests
# compile AND RUN, which is impossible here -- so ggml falls back to whatever -march says
# and silently drops its fast kernels. That flag is worth more than any other choice on
# this page: adding FEAT_I8MM took Qwen3 4B decode from 5.3 to 12.4 tok/s on an 8 Elite
# Gen 5, a 2.3x swing from one instruction set.
#
# But a single flag cannot serve one APK. An i8mm binary SIGILLs on a Snapdragon 865,
# which minSdk 26 still allows, so picking the fast one abandons those devices and picking
# the safe one abandons the speed.
#
# So: GGML_CPU_ALL_VARIANTS. ggml builds one libggml-cpu-*.so per feature level, each
# exporting ggml_backend_score(); at startup the loader dlopens them all, asks each what it
# scores on THIS cpu, and keeps the winner. Seven variants cost ~6 MB stripped, which is
# the whole price of never choosing wrong.
#
# That requires GGML_BACKEND_DL, which requires BUILD_SHARED_LIBS -- so this SDK is shared
# objects, not the static archives the Harmony build produces, and the APK must ship them
# as jniLibs with extractNativeLibs=true (the loader scans a directory for real files; a
# library still inside the zip is not one). See app/build.gradle.kts.
#
# Env overrides:
#   LLAMA_SRC          llama.cpp checkout (default: llama.cpp beside the repo)
#   LLAMA_SDK_DIR      where to assemble the SDK (default: android/prebuilt/llama-sdk/<abi>)
#   ANDROID_NDK_HOME   NDK to use (default: resolved from ANDROID_HOME, as build-prebuilt.sh does)
# ---------------------------------------------------------------------------
set -euo pipefail

# Resolved BEFORE the cd: --help reads this file's own header back out, and a relative
# BASH_SOURCE stops resolving once the working directory moves.
SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
cd "$(dirname "$SCRIPT")"
SCRIPT_DIR="$(pwd)"

PINNED_NDK="27.0.12077973"

ABI="" ; CLEAN=""
for tok in "$@"; do
    case "$tok" in
        clean)     CLEAN=1 ;;
        -h|--help) sed -n "2,36p" "$SCRIPT" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)         ABI="$tok" ;;
    esac
done
ABI="${ABI:-arm64-v8a}"
# Rejected here rather than at -DANDROID_ABI, where an unknown value fails deep inside the
# toolchain file with nothing pointing back at the argument.
case "$ABI" in
  arm64-v8a|x86_64) ;;
  *) echo "[build-llama] abi must be arm64-v8a|x86_64 (got '$ABI')" >&2; exit 2 ;;
esac

# --- llama.cpp checkout -------------------------------------------------------
# Beside the repo, matching build-llama.cmd, harmony/build-llama.* and qt/build-qt.* --
# one checkout serves all of them. Not cloned for you: this build wants a checkout you
# can pin and re-measure against, not one silently refreshed under it.
LLAMA_SRC="${LLAMA_SRC:-$(cd "$SCRIPT_DIR/../.." && pwd)/llama.cpp}"
if [ ! -f "$LLAMA_SRC/include/llama.h" ]; then
    echo "[build-llama] no llama.cpp at \"$LLAMA_SRC\"." >&2
    echo "[build-llama] clone it, or set LLAMA_SRC to an existing checkout:" >&2
    echo "[build-llama]   git clone --depth 1 https://github.com/ggml-org/llama.cpp \"$LLAMA_SRC\"" >&2
    exit 1
fi

# --- NDK ----------------------------------------------------------------------
# Same resolution order as build-prebuilt.sh, deliberately: two scripts that disagree
# about which NDK is "the" one produce an SDK the app cannot link against.
#
# No space-free mirror here, unlike build-llama.cmd. That mirror exists to dodge Windows
# 8.3 short names mangling clang++.exe into CLANG_~1.EXE; nothing on macOS or Linux does
# that, so a spaced path is merely a path.
NDK_HOME="${ANDROID_NDK_HOME:-}"
if [ -z "$NDK_HOME" ] || [ ! -d "$NDK_HOME" ]; then
    SDK="${ANDROID_HOME:-}"
    if [ -z "$SDK" ]; then
        case "$(uname -s)" in
            Darwin) SDK="$HOME/Library/Android/sdk" ;;
            *)      SDK="$HOME/Android/Sdk" ;;
        esac
    fi
    NDK_DIR="$SDK/ndk"
    if [ -d "$NDK_DIR/$PINNED_NDK" ]; then
        NDK_HOME="$NDK_DIR/$PINNED_NDK"
    elif [ -d "$NDK_DIR" ]; then
        NDK_HOME="$(ls -1d "$NDK_DIR"/*/ 2>/dev/null | sort -Vr | head -1 | sed 's:/*$::')"
    fi
fi
TOOLCHAIN="$NDK_HOME/build/cmake/android.toolchain.cmake"
if [ ! -f "$TOOLCHAIN" ]; then
    echo "[build-llama] Android NDK not found at \"$NDK_HOME\"." >&2
    echo "[build-llama] set ANDROID_NDK_HOME (or ANDROID_HOME)." >&2
    exit 1
fi

# The SDK ships cmake and ninja under cmake/<ver>/bin; fall back to the host's.
# Where the SDK is: ANDROID_HOME, else Gradle's own record of it (local.properties
# `sdk.dir`, written by Android Studio, whose values are .properties-escaped), else two
# levels up from the NDK (sdk/ndk/<ver> -> sdk). Same chain as build-llama.cmd.
CMAKE="" ; NINJA=""
SDK_ROOT="${ANDROID_HOME:-}"
if [ -z "$SDK_ROOT" ] && [ -f "$SCRIPT_DIR/local.properties" ]; then
    SDK_ROOT="$(sed -n 's/^sdk\.dir=//p' "$SCRIPT_DIR/local.properties" | sed 's/\\\(.\)/\1/g')"
fi
[ -n "$SDK_ROOT" ] || SDK_ROOT="$(dirname "$(dirname "$NDK_HOME")")"
for d in "$SDK_ROOT"/cmake/*/bin; do
    [ -x "$d/cmake" ] && CMAKE="$d/cmake"
    [ -x "$d/ninja" ] && NINJA="$d/ninja"
done
[ -n "$CMAKE" ] || CMAKE="$(command -v cmake || true)"
[ -n "$NINJA" ] || NINJA="$(command -v ninja || true)"
if [ -z "$CMAKE" ] || [ -z "$NINJA" ]; then
    echo "[build-llama] need cmake and ninja (the SDK's cmake/<ver>/bin, or on PATH)." >&2
    exit 1
fi

BUILD="$SCRIPT_DIR/.llama-build/$ABI"
SDK_OUT="${LLAMA_SDK_DIR:-$SCRIPT_DIR/prebuilt/llama-sdk/$ABI}"

if [ -n "$CLEAN" ]; then
    echo "[build-llama] cleaning"
    rm -rf "$BUILD" "$SDK_OUT"
fi

echo "[build-llama] abi=$ABI"
echo "[build-llama] cpu variants: all (chosen at runtime by ggml_backend_score)"
echo "[build-llama] ndk=$NDK_HOME"

# GGML_NATIVE=OFF because probing the host says nothing about the target.
# LLAMA_CURL=OFF: the app downloads models itself; curl here would drag in a second,
# differently-configured copy of a dependency the core already links.
# c++_shared to match app/build.gradle.kts, or two STLs meet at link time.
#
# max-page-size=16384 for the same reason libmirobody.so needs it: a .so whose LOAD
# segments are 4 KB-aligned will not load at all on an Android 15+ device running a 16 KB
# kernel page size. ggml's own build does not set it, and nothing warns.
#
# MODULE as well as SHARED, and that is not belt-and-braces: ggml builds the CPU variants
# with add_library(... MODULE), which takes CMAKE_MODULE_LINKER_FLAGS and ignores the
# SHARED one entirely. Setting only SHARED aligns libllama/libggml to 16 KB and leaves all
# seven variants at 4 KB -- i.e. exactly the libraries this whole build exists to ship,
# silently unloadable. Verify with readelf, not by reading this.
"$CMAKE" -S "$LLAMA_SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM=android-26 \
  -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
  -DCMAKE_MODULE_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
  -DBUILD_SHARED_LIBS=ON \
  -DGGML_BACKEND_DL=ON \
  -DGGML_CPU_ALL_VARIANTS=ON \
  -DGGML_NATIVE=OFF \
  -DGGML_OPENMP=OFF \
  -DLLAMA_CURL=OFF \
  -DLLAMA_BUILD_TESTS=OFF \
  -DLLAMA_BUILD_EXAMPLES=OFF \
  -DLLAMA_BUILD_SERVER=OFF \
  -DLLAMA_BUILD_TOOLS=OFF

# The `llama` target pulls ggml, ggml-base and every ggml-cpu-* variant with it.
"$CMAKE" --build "$BUILD" --target llama

echo "[build-llama] assembling $SDK_OUT"
rm -rf "$SDK_OUT"
mkdir -p "$SDK_OUT/include" "$SDK_OUT/lib"
cp "$LLAMA_SRC/include/llama.h" "$SDK_OUT/include/"
cp "$LLAMA_SRC"/ggml/include/*.h "$SDK_OUT/include/"

# Stripped on the way in: unstripped these are 79 MB against 9.8 MB, and every one of them
# is packaged into the APK as-is (jniLibs are not run through AGP's stripper the way an
# externalNativeBuild output is).
STRIP=""
for host in darwin-x86_64 linux-x86_64; do
    [ -x "$NDK_HOME/toolchains/llvm/prebuilt/$host/bin/llvm-strip" ] &&
        STRIP="$NDK_HOME/toolchains/llvm/prebuilt/$host/bin/llvm-strip"
done
for f in "$BUILD"/bin/*.so; do
    if [ -n "$STRIP" ]; then
        "$STRIP" --strip-unneeded -o "$SDK_OUT/lib/$(basename "$f")" "$f"
    else
        cp "$f" "$SDK_OUT/lib/"
    fi
done

echo "[build-llama] done: $SDK_OUT"
echo "[build-llama] the app picks this up by itself -- rebuild the APK to link it in."
