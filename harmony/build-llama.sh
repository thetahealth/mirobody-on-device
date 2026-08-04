#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Cross-compile llama.cpp for HarmonyOS and assemble an SDK dir the app links.
# The macOS/Linux counterpart of build-llama.cmd (same tokens, defaults, flags and
# output layout), as build-prebuilt.sh is of build-prebuilt.cmd.
#
#   ./build-llama.sh [abi] [backend] [arch] [clean]      (tokens in any order)
#
#     abi      arm64-v8a (default) | x86_64
#     backend  cpu (default) | vulkan. Each gets its OWN cached SDK, so switching
#              cannot silently keep linking the previous one.
#     arch     the -march feature set for the CPU backend. Default is read off a
#              REAL DEVICE, not guessed: Kirin 9020 reports fp16+dotprod+i8mm+bf16
#              +sve via AT_HWCAP (see nativeLocalStatus). Pass `baseline` for plain
#              armv8-a, or any GGML_CPU_ARM_ARCH string.
#     clean    discard the cached build tree and SDK first
#
# WHY THIS EXISTS: a cross build with no -march flags silently lands on baseline
# armv8-a. CMake's feature probes compile-and-run, which is impossible when
# cross-compiling, so HAVE_MATMUL_INT8 / HAVE_FP16_VECTOR_ARITHMETIC / HAVE_SVE all
# "fail" and ggml quietly drops the fast kernels -- on a Kirin 9020 that left
# dotprod, i8mm, fp16 and SVE unused. GGML_CPU_ARM_ARCH states the target's features
# instead of probing for them.
#
# Output: ~/opt/llama-sdk-ohos-<abi>-<backend>/{include,lib} (LLAMA_SDK_DIR overrides),
# static archives -- a HAP has nowhere to put a companion .so, so the engine is linked
# into libmirobody.so. Feed it to the app with -DLLAMA_CPP_DIR in
# entry/build-profile.json5; this script prints the line to paste.
#
# USE `cpu`. The vulkan backend builds and genuinely runs (it registers an igpu on a
# Kirin 9020, so this is not a silent fallback) and it loses on every axis: ~5x slower
# load, ~24x slower prefill, ~3.5x slower decode, plus ~50 MB of SPIR-V in the HAP.
# The measurement table and the structural reason (offload copies every tensor into
# device memory that shares its bandwidth with the CPU) are in build-llama.cmd -- the
# one place they live, so they cannot drift between the two scripts. Kept buildable
# because a different SoC or driver could flip this; re-measure before believing it did.
#
# Env overrides:
#   LLAMA_SRC      llama.cpp checkout (default: ~/.cache/mirobody/llama.cpp)
#   LLAMA_SDK_DIR  where to assemble the SDK (default: ~/opt/llama-sdk-ohos-<abi>-<backend>)
#   OHOS_SDK_ROOT  the SDK dir CONTAINING native/ (same meaning as in build-prebuilt.sh)
#   DEVECO_HOME    DevEco Studio install, as on Windows; its sdk/default/openharmony is used
#   GLSLC          shader compiler for the vulkan backend (else $VULKAN_SDK/bin, else PATH)
# ---------------------------------------------------------------------------
set -euo pipefail

# Resolve our own path BEFORE the cd: --help reads the header block back out of this
# file, and a relative BASH_SOURCE stops resolving once the working directory moves.
SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
cd "$(dirname "$SCRIPT")"
SCRIPT_DIR="$(pwd)"

# Tokens are position-independent apart from abi-before-arch, because
# `build-llama.sh arm64-v8a clean` is what one naturally types and treating `clean`
# as the arch silently produced `-march=clean`.
ABI="" ; ARCH="" ; ARCH_SET="" ; CLEAN="" ; BACKEND="cpu"
for tok in "$@"; do
    case "$tok" in
        clean)       CLEAN=1 ;;
        cpu|vulkan)  BACKEND="$tok" ;;
        baseline)    ARCH="" ; ARCH_SET=1 ;;
        -h|--help)   sed -n "2,44p" "$SCRIPT" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)
            if [ -z "$ABI" ]; then ABI="$tok"; else ARCH="$tok"; ARCH_SET=1; fi
            ;;
    esac
done

ABI="${ABI:-arm64-v8a}"
# The sysroot's per-target lib dir (needed for the vulkan loader below).
case "$ABI" in
  arm64-v8a) OHOS_TRIPLE="aarch64-linux-ohos" ;;
  x86_64)    OHOS_TRIPLE="x86_64-linux-ohos" ;;
  *) echo "[build-llama] abi must be arm64-v8a|x86_64 (got '$ABI')" >&2; exit 2 ;;
esac

# Default is the best measured config on a Kirin 9020: vs baseline armv8-a it is
# ~2.95x prefill and ~1.42x decode. `+sve` is deliberately absent -- the device has
# SVE1, and adding it REGRESSED prefill by ~34% (measured), apparently by displacing
# the i8mm SMMLA kernels with SVE ones that have no width advantage at 128-bit
# vectors. Pass an explicit arch to try something else.
[ -n "$ARCH_SET" ] || ARCH="armv8.6-a+i8mm+bf16+dotprod+fp16"

# --- llama.cpp source: cloned once by hand, reused ---------------------------
# Not cloned for you, as in build-llama.cmd: this build wants a checkout you can
# pin and re-measure against, not one silently refreshed under it.
LLAMA_SRC="${LLAMA_SRC:-$HOME/.cache/mirobody/llama.cpp}"
if [ ! -f "$LLAMA_SRC/include/llama.h" ]; then
    echo "[build-llama] no llama.cpp at \"$LLAMA_SRC\"." >&2
    echo "[build-llama] clone it, or set LLAMA_SRC to an existing checkout:" >&2
    echo "[build-llama]   git clone --depth 1 https://github.com/ggml-org/llama.cpp \"$LLAMA_SRC\"" >&2
    exit 1
fi

# --- OHOS native SDK: toolchain file + its own cmake/ninja -------------------
# OHOS_SDK_ROOT names the directory CONTAINING native/, exactly as build-prebuilt.sh
# uses it. DEVECO_HOME is accepted too so the same env var works on both platforms.
if [ -z "${OHOS_SDK_ROOT:-}" ] && [ -n "${DEVECO_HOME:-}" ]; then
    OHOS_SDK_ROOT="$DEVECO_HOME/sdk/default/openharmony"
fi
if [ -z "${OHOS_SDK_ROOT:-}" ]; then
    for d in \
      "$HOME/.local/opt/ohos/command-line-tools/sdk/default/openharmony" \
      "$HOME/command-line-tools/sdk/default/openharmony" \
      "/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony"
    do
        if [ -f "$d/native/build/cmake/ohos.toolchain.cmake" ]; then OHOS_SDK_ROOT="$d"; break; fi
    done
fi
NATIVE="${OHOS_SDK_ROOT:-}/native"
TOOLCHAIN="$NATIVE/build/cmake/ohos.toolchain.cmake"
if [ ! -f "$TOOLCHAIN" ]; then
    echo "[build-llama] OHOS toolchain not found at \"$TOOLCHAIN\"." >&2
    echo "[build-llama] set OHOS_SDK_ROOT (dir containing native/) or DEVECO_HOME." >&2
    exit 1
fi

# The SDK ships its own cmake/ninja beside the toolchain; fall back to the host's.
CMAKE="$NATIVE/build-tools/cmake/bin/cmake"
NINJA="$NATIVE/build-tools/cmake/bin/ninja"
[ -x "$CMAKE" ] || CMAKE="$(command -v cmake || true)"
[ -x "$NINJA" ] || NINJA="$(command -v ninja || true)"
if [ -z "$CMAKE" ] || [ -z "$NINJA" ]; then
    echo "[build-llama] need cmake and ninja (the SDK's build-tools/cmake/bin, or on PATH)." >&2
    exit 1
fi

# Ninja must be findable BY NAME, not just via CMAKE_MAKE_PROGRAM: the Vulkan backend
# builds its shader generator as a HOST tool through ExternalProject, and that nested
# configure inherits the generator but not the make program, so it fails with "unable
# to find a build program corresponding to Ninja".
PATH="$(cd "$(dirname "$NINJA")" && pwd):$PATH"
export PATH

BUILD="$SCRIPT_DIR/.llama-build/$ABI-$BACKEND"
# One SDK per backend: a switch must not silently link the previous build.
SDK="${LLAMA_SDK_DIR:-$HOME/opt/llama-sdk-ohos-$ABI-$BACKEND}"

if [ -n "$CLEAN" ]; then
    echo "[build-llama] cleaning"
    rm -rf "$BUILD" "$SDK"
fi

echo "[build-llama] abi=$ABI backend=$BACKEND"
if [ -z "$ARCH" ]; then echo "[build-llama] arch=baseline armv8-a"; else echo "[build-llama] arch=$ARCH"; fi

# GGML_NATIVE=OFF because probing the host says nothing about the target.
# LLAMA_CURL=OFF: the app downloads models itself; curl here would drag in a second,
# differently-configured copy of a dependency the core already links.
# Static libs only -- see the header comment.
CFG=(
  -S "$LLAMA_SRC" -B "$BUILD" -G Ninja
  -DCMAKE_MAKE_PROGRAM="$NINJA"
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
  -DOHOS_ARCH="$ABI"
  -DCMAKE_BUILD_TYPE=Release
  -DBUILD_SHARED_LIBS=OFF
  -DGGML_NATIVE=OFF
  -DGGML_OPENMP=OFF
  -DLLAMA_CURL=OFF
  -DLLAMA_BUILD_TESTS=OFF
  -DLLAMA_BUILD_EXAMPLES=OFF
  -DLLAMA_BUILD_SERVER=OFF
  -DLLAMA_BUILD_TOOLS=OFF
)
[ -n "$ARCH" ] && CFG+=(-DGGML_CPU_ARM_ARCH="$ARCH")

# Vulkan: OHOS ships the loader and headers in its own sysroot, so the only piece that
# must come from the host is glslc -- the shader compiler runs at BUILD time and is not
# part of the cross toolchain. Point CMake at the sysroot's Vulkan explicitly; left to
# itself it would find the HOST SDK's and link the wrong ABI.
if [ "$BACKEND" = "vulkan" ]; then
    GLSLC="${GLSLC:-}"
    if [ -z "$GLSLC" ] && [ -n "${VULKAN_SDK:-}" ] && [ -x "$VULKAN_SDK/bin/glslc" ]; then
        GLSLC="$VULKAN_SDK/bin/glslc"
    fi
    [ -n "$GLSLC" ] || GLSLC="$(command -v glslc || true)"
    if [ -z "$GLSLC" ]; then
        echo "[build-llama] vulkan needs glslc; install the Vulkan SDK or set GLSLC." >&2
        exit 1
    fi
    echo "[build-llama] glslc=$GLSLC"
    # ggml-vulkan also wants SPIRV-Headers, found via $ENV{VULKAN_SDK}. Those are HOST
    # build-time headers for compiling shaders, not target libraries, but the OHOS
    # toolchain sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE to ONLY and hides anything
    # outside the sysroot -- hence the override below.
    VULKAN_SDK="$(cd "$(dirname "$GLSLC")/.." && pwd)"
    export VULKAN_SDK
    echo "[build-llama] VULKAN_SDK=$VULKAN_SDK"
    # Headers come from the HOST SDK, the loader from the OHOS sysroot: ggml-vulkan
    # includes vulkan.hpp (C++ bindings) and the sysroot ships only the C headers.
    # Mixing is sound -- vulkan.hpp is header-only, the ABI is stable, and the loader
    # negotiates its version at runtime -- but watch it if a device reports a much
    # older Vulkan than the host SDK.
    CFG+=(
      -DGGML_VULKAN=ON
      -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH
      -DVulkan_GLSLC_EXECUTABLE="$GLSLC"
      -DVulkan_INCLUDE_DIR="$VULKAN_SDK/include"
      -DVulkan_LIBRARY="$NATIVE/sysroot/usr/lib/$OHOS_TRIPLE/libvulkan.so"
      -DSPIRV-Headers_DIR="$VULKAN_SDK/lib/cmake/SPIRV-Headers"
    )
fi

"$CMAKE" "${CFG[@]}"

# Only the libraries: upstream's single `llama-app` CLI target links impl libs that the
# OFF switches above remove, so asking for it would fail the link -- and we want the
# library, not the CLI.
TARGETS=(llama ggml ggml-cpu ggml-base)
[ "$BACKEND" = "vulkan" ] && TARGETS+=(ggml-vulkan)
"$CMAKE" --build "$BUILD" --target "${TARGETS[@]}"

echo "[build-llama] assembling $SDK"
mkdir -p "$SDK/include" "$SDK/lib"
cp "$LLAMA_SRC/include/llama.h" "$SDK/include/"
cp "$LLAMA_SRC"/ggml/include/*.h "$SDK/include/"
cp "$BUILD/src/libllama.a" "$SDK/lib/"
cp "$BUILD/ggml/src/libggml.a" "$BUILD/ggml/src/libggml-cpu.a" "$BUILD/ggml/src/libggml-base.a" "$SDK/lib/"
if [ "$BACKEND" = "vulkan" ]; then
    # The backend lives in its own subdirectory and its own archive.
    find "$BUILD/ggml/src" -name libggml-vulkan.a -exec cp {} "$SDK/lib/" \;
fi

echo "[build-llama] done: $SDK"
echo "[build-llama] point the app at it in entry/build-profile.json5:"
echo "[build-llama]   \"arguments\": \"-DLLAMA_CPP_DIR=$SDK\""
