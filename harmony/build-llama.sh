#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Cross-compile llama.cpp for HarmonyOS and assemble an SDK dir the app links.
# The macOS/Linux counterpart of build-llama.cmd (same tokens, defaults, flags and
# output layout), as build-prebuilt.sh is of build-prebuilt.cmd.
#
#   ./build-llama.sh [abi] [arch] [clean]               (tokens in any order)
#
#     abi      arm64-v8a (default) | x86_64
#     arch     the -march feature set. Default is read off a REAL DEVICE, not
#              guessed: Kirin 9020 reports fp16+dotprod+i8mm+bf16+sve via AT_HWCAP
#              (see nativeLocalStatus). Pass `baseline` for plain armv8-a, or any
#              GGML_CPU_ARM_ARCH string.
#     clean    discard the cached build tree and SDK first
#
# WHY THIS EXISTS: a cross build with no -march flags silently lands on baseline
# armv8-a. CMake's feature probes compile-and-run, which is impossible when
# cross-compiling, so HAVE_MATMUL_INT8 / HAVE_FP16_VECTOR_ARITHMETIC / HAVE_SVE all
# "fail" and ggml quietly drops the fast kernels -- on a Kirin 9020 that left
# dotprod, i8mm, fp16 and SVE unused. GGML_CPU_ARM_ARCH states the target's features
# instead of probing for them.
#
# Output: harmony/prebuilt/llama-sdk/<abi>/{include,lib}, which the app's CMakeLists
# finds BY ITSELF -- the same contract as the vcpkg deps in harmony/prebuilt/<abi>, and
# deliberately under the same parent: `prebuilt/` should mean every prebuilt artifact,
# not just the ones vcpkg made. Build it and the on-device lane is on; delete it and the
# core compiles its stub and the app still builds. Nothing to paste into
# entry/build-profile.json5. Static archives: a HAP has nowhere to put a companion .so,
# so the engine links into libmirobody.so.
#
# CPU ONLY, deliberately -- there is no backend option. The vulkan backend built and
# genuinely ran (it registered an igpu on a Kirin 9020, so this was not a silent
# fallback) and it lost on every axis: ~5x slower load, ~24x slower prefill, ~3.5x
# slower decode, plus ~50 MB of SPIR-V in the HAP. The measurement table and the
# structural reason (offload copies every tensor into device memory that shares its
# bandwidth with the CPU) are in build-llama.cmd -- the one place they live, so they
# cannot drift between the two scripts. The plumbing is removed rather than switched
# off; `git log -p -- harmony/build-llama.sh` has it if it is ever worth re-measuring.
#
# Env overrides:
#   LLAMA_SRC      llama.cpp checkout (default: llama.cpp beside the repo)
#   LLAMA_SDK_DIR  where to assemble the SDK (default: harmony/prebuilt/llama-sdk/<abi>).
#                  Moving it out of the tree means naming it with -DLLAMA_CPP_DIR in
#                  entry/build-profile.json5 -- the auto-detect only looks in the
#                  default place.
#   OHOS_SDK_ROOT  the SDK dir CONTAINING native/ (same meaning as in build-prebuilt.sh)
#   DEVECO_HOME    DevEco Studio install, as on Windows; its sdk/default/openharmony is used
# ---------------------------------------------------------------------------
set -euo pipefail

# Resolve our own path BEFORE the cd: --help reads the header block back out of this
# file, and a relative BASH_SOURCE stops resolving once the working directory moves.
SCRIPT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
cd "$(dirname "$SCRIPT")"
SCRIPT_DIR="$(pwd)"

# Tokens are position-independent apart from abi-before-arch, because
# `build-llama.sh arm64-v8a clean` is what one naturally types and treating `clean`
# as the arch silently produced `-march=clean`. `cpu` is accepted and ignored: it
# used to be the backend token, so it is all over older notes, and as an arch it
# would mean `-march=cpu`.
ABI="" ; ARCH="" ; ARCH_SET="" ; CLEAN=""
for tok in "$@"; do
    case "$tok" in
        clean)       CLEAN=1 ;;
        cpu)         ;;
        vulkan)
            echo "[build-llama] there is no vulkan backend here any more -- CPU only." >&2
            echo "[build-llama] the measurements that decided it are in build-llama.cmd." >&2
            exit 2
            ;;
        baseline)    ARCH="" ; ARCH_SET=1 ;;
        -h|--help)   sed -n "2,46p" "$SCRIPT" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)
            if [ -z "$ABI" ]; then ABI="$tok"; else ARCH="$tok"; ARCH_SET=1; fi
            ;;
    esac
done

ABI="${ABI:-arm64-v8a}"
# Rejected here instead of at -DOHOS_ARCH, where an unknown ABI fails deep in the
# toolchain file with nothing pointing back at the argument.
case "$ABI" in
  arm64-v8a|x86_64) ;;
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
# Beside the repo, matching build-llama.cmd, qt/build-qt.cmd and
# fine-tuning/train_units.py -- one checkout serves all of them.
LLAMA_SRC="${LLAMA_SRC:-$(cd "$SCRIPT_DIR/../.." && pwd)/llama.cpp}"
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

BUILD="$SCRIPT_DIR/.llama-build/$ABI"
# In-tree under harmony/prebuilt/, for the same reason the vcpkg deps are: the app's
# CMakeLists resolves it by repo-relative path, so there is no machine-local absolute
# path to paste into a tracked file. One /prebuilt rule in .gitignore covers both, and
# build-prebuilt only ever replaces its own prebuilt/<abi>.
SDK="${LLAMA_SDK_DIR:-$SCRIPT_DIR/prebuilt/llama-sdk/$ABI}"

if [ -n "$CLEAN" ]; then
    echo "[build-llama] cleaning"
    rm -rf "$BUILD" "$SDK"
fi

echo "[build-llama] abi=$ABI"
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

"$CMAKE" "${CFG[@]}"

# Only the libraries: upstream's single `llama-app` CLI target links impl libs that the
# OFF switches above remove, so asking for it would fail the link -- and we want the
# library, not the CLI.
"$CMAKE" --build "$BUILD" --target llama ggml ggml-cpu ggml-base

echo "[build-llama] assembling $SDK"
mkdir -p "$SDK/include" "$SDK/lib"
cp "$LLAMA_SRC/include/llama.h" "$SDK/include/"
cp "$LLAMA_SRC"/ggml/include/*.h "$SDK/include/"
cp "$BUILD/src/libllama.a" "$SDK/lib/"
cp "$BUILD/ggml/src/libggml.a" "$BUILD/ggml/src/libggml-cpu.a" "$BUILD/ggml/src/libggml-base.a" "$SDK/lib/"

echo "[build-llama] done: $SDK"
if [ -n "${LLAMA_SDK_DIR:-}" ]; then
    echo "[build-llama] that is NOT the auto-detected location, so name it in"
    echo "[build-llama] entry/build-profile.json5:"
    echo "[build-llama]   \"arguments\": \"-DLLAMA_CPP_DIR=$SDK\""
else
    echo "[build-llama] the app picks this up by itself -- rebuild the HAP to link it in."
fi
