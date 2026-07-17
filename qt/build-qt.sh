#!/usr/bin/env bash
# build-qt.sh -- build the Qt Quick desktop client (qt/) on Linux/macOS, optionally
# with the on-device engine (llama.cpp). build-qt only *builds*: it never downloads a
# model. Models are chosen at runtime in the app (gear -> On-device AI); the app ships
# with a default. So the only build choice is the compute backend, which this script
# builds + caches the llama.cpp SDK for (per backend).
#
# qt/CMakeLists.txt has no project() call, so we generate an add_subdirectory wrapper.
# The exe lands in build-qt/app/ next to its generated Mirobody/ QML module dir.
#
# Usage: ./build-qt.sh [check] [clean] [deploy] [<backend>]  (any order)
#   check      detect this machine's GPU/SDKs and recommend a backend, then exit
#   <backend>  cpu | avx2 | vulkan | cuda  -- giving one ENABLES the on-device engine:
#                cpu    portable CPU baseline (GGML_NATIVE=OFF)
#                avx2   CPU tuned to this machine (GGML_NATIVE=ON)
#                vulkan GPU via Vulkan (needs the Vulkan SDK)
#                cuda   NVIDIA GPU via CUDA (needs the CUDA Toolkit; fastest on NVIDIA)
#   clean      wipe caches / force rebuild (needed to turn on-device OFF)
#   deploy     macOS: macdeployqt the .app. Linux: prints guidance.
#   -h/--help  show this help
#
# Env overrides:
#   QT_PREFIX      Qt kit dir (CMAKE_PREFIX_PATH); else rely on CMake finding Qt6
#   LLAMA_SRC      llama.cpp checkout (default: ~/.cache/mirobody/llama.cpp; cloned if missing)
#   LLAMA_CPP_DIR  use a prebuilt llama.cpp SDK (include/,lib/) instead of building one
set -euo pipefail

# This script lives in qt/. QT_SRC is its own dir; the build output goes to the
# repo root (alongside build/, build-legacy/, ...), covered by .gitignore build-*/.
QT_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$QT_SRC/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build-qt"
WRAP_DIR="$PROJECT_DIR/build-qt-wrap"
CACHE_DIR="$BUILD_DIR"
EXE="$BUILD_DIR/app/mirobody_qt"

CLEAN="" ; DEPLOY="" ; BACKEND="" ; CHECK=""
for tok in "$@"; do
    case "$tok" in
        check)  CHECK=1 ;;
        clean)  CLEAN=1 ;;
        deploy) DEPLOY=1 ;;
        cpu|avx2|vulkan|cuda) BACKEND="$tok" ;;
        -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "[build-qt] Unknown token: $tok" >&2; exit 2 ;;
    esac
done

# `check`: report which on-device backends this machine can build/run, then exit.
if [ -n "$CHECK" ]; then
    if command -v lspci >/dev/null 2>&1; then
        gpus="$(lspci 2>/dev/null | grep -iE 'vga|3d controller|display' | sed 's/.*: //' | paste -sd'; ' -)"
    elif [ "$(uname)" = "Darwin" ]; then
        gpus="$(system_profiler SPDisplaysDataType 2>/dev/null | awk -F': ' '/Chipset Model/{print $2}' | paste -sd'; ' -)"
    fi
    [ -z "${gpus:-}" ] && gpus="unknown"
    hasnv="" ; { command -v nvidia-smi >/dev/null 2>&1 || echo "$gpus" | grep -qi nvidia; } && hasnv=1
    vk_ready="" ; { [ -n "${VULKAN_SDK:-}" ] || command -v glslc >/dev/null 2>&1; } && vk_ready=1
    cuda_ready="" ; { [ -n "${CUDA_PATH:-}" ] || command -v nvcc >/dev/null 2>&1; } && cuda_ready=1
    echo
    echo "[build-qt] On-device (Gemma) backend options for this machine:"
    echo "  GPU detected : $gpus"
    echo
    echo "  cpu    : available          portable CPU baseline"
    echo "  avx2   : available          native CPU build (faster)"
    [ -n "$vk_ready" ] && echo "  vulkan : READY              Vulkan SDK found" \
                       || echo "  vulkan : needs Vulkan SDK   https://vulkan.lunarg.com/"
    if [ -n "$hasnv" ]; then
        [ -n "$cuda_ready" ] && echo "  cuda   : READY              NVIDIA GPU + CUDA Toolkit" \
                             || echo "  cuda   : needs CUDA Toolkit https://developer.nvidia.com/cuda-downloads"
    else
        echo "  cuda   : n/a                no NVIDIA GPU"
    fi
    rec="avx2"; [ -n "$vk_ready" ] && rec="vulkan"; { [ -n "$hasnv" ] && [ -n "$cuda_ready" ]; } && rec="cuda"
    echo
    echo "  Recommended  : $rec"
    echo "  Run          : qt/build-qt.sh $rec deploy"
    echo
    exit 0
fi

# Build (or reuse) a llama.cpp SDK for $BACKEND; sets LLAMA_CPP_DIR.
ensure_llama() {
    if [ -n "${LLAMA_CPP_DIR:-}" ]; then
        echo "[build-qt] using preset LLAMA_CPP_DIR=$LLAMA_CPP_DIR"; return 0
    fi
    local cache="$HOME/.cache/mirobody"
    LLAMA_SRC="${LLAMA_SRC:-$cache/llama.cpp}"
    LLAMA_CPP_DIR="$cache/llama-sdk-$BACKEND"
    if [ -z "$CLEAN" ] && { [ -f "$LLAMA_CPP_DIR/lib/libllama.so" ] || [ -f "$LLAMA_CPP_DIR/lib/libllama.dylib" ]; }; then
        echo "[build-qt] reusing cached llama SDK: $LLAMA_CPP_DIR"; return 0
    fi
    if [ "$BACKEND" = "vulkan" ] && [ -z "${VULKAN_SDK:-}" ]; then
        echo "[build-qt] 'vulkan' backend needs the Vulkan SDK (set VULKAN_SDK)." >&2; return 1
    fi
    if [ "$BACKEND" = "cuda" ] && [ -z "${CUDA_PATH:-}" ] && ! command -v nvcc >/dev/null 2>&1; then
        echo "[build-qt] 'cuda' backend needs the CUDA Toolkit (nvcc on PATH or CUDA_PATH set)." >&2; return 1
    fi
    [ -f "$LLAMA_SRC/CMakeLists.txt" ] || {
        echo "[build-qt] cloning llama.cpp -> $LLAMA_SRC"
        git clone --depth 1 https://github.com/ggml-org/llama.cpp "$LLAMA_SRC"
    }
    local lflags="-DGGML_NATIVE=OFF"
    [ "$BACKEND" = "avx2" ]   && lflags="-DGGML_NATIVE=ON"
    [ "$BACKEND" = "vulkan" ] && lflags="-DGGML_VULKAN=ON"
    [ "$BACKEND" = "cuda" ]   && lflags="-DGGML_CUDA=ON"
    local lbuild="$LLAMA_SRC/build-$BACKEND"
    echo "[build-qt] building llama.cpp ($BACKEND) -- first time takes a few minutes..."
    cmake -S "$LLAMA_SRC" -B "$lbuild" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF \
        -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=OFF $lflags
    cmake --build "$lbuild" --target llama
    mkdir -p "$LLAMA_CPP_DIR/include" "$LLAMA_CPP_DIR/lib"
    cp "$LLAMA_SRC"/include/*.h "$LLAMA_CPP_DIR/include/"
    cp "$LLAMA_SRC"/ggml/include/*.h "$LLAMA_CPP_DIR/include/"
    find "$lbuild" \( -name 'libllama.*' -o -name 'libggml*.*' \) -exec cp {} "$LLAMA_CPP_DIR/lib/" \;
    echo "[build-qt] llama SDK ready: $LLAMA_CPP_DIR"
}

ONDEV_ARGS=()
if [ -n "$BACKEND" ]; then
    ensure_llama
    # The default model is baked into the app; models are managed at runtime, not here.
    ONDEV_ARGS=(-DMIROBODY_ONDEVICE_LLM=ON -DLLAMA_CPP_DIR="$LLAMA_CPP_DIR")
    echo "[build-qt] on-device: backend=$BACKEND"
fi

[ -n "$CLEAN" ] && rm -rf "$CACHE_DIR"
mkdir -p "$WRAP_DIR"
cat > "$WRAP_DIR/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.19)
project(mirobody_qt_standalone LANGUAGES C CXX)
add_subdirectory("$QT_SRC" app)
EOF

CMAKE_ARGS=(-S "$WRAP_DIR" -B "$CACHE_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release)
[ -n "${QT_PREFIX:-}" ] && CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$QT_PREFIX")
cmake "${CMAKE_ARGS[@]}" "${ONDEV_ARGS[@]}"
cmake --build "$CACHE_DIR" --target mirobody_qt

# On-device build needs the llama.cpp runtime libs findable next to the exe.
if [ -n "$BACKEND" ]; then
    cp "$LLAMA_CPP_DIR"/lib/*.so    "$BUILD_DIR/app/" 2>/dev/null || true
    cp "$LLAMA_CPP_DIR"/lib/*.dylib "$BUILD_DIR/app/" 2>/dev/null || true
fi
echo "[build-qt] Built: $EXE"

if [ -n "$DEPLOY" ]; then
    if [ "$(uname)" = "Darwin" ]; then
        MACDEPLOY="${QT_PREFIX:-}/bin/macdeployqt"
        [ -x "$MACDEPLOY" ] || MACDEPLOY="$(command -v macdeployqt || true)"
        if [ -n "$MACDEPLOY" ] && [ -d "$BUILD_DIR/app/mirobody_qt.app" ]; then
            "$MACDEPLOY" "$BUILD_DIR/app/mirobody_qt.app" -qmldir="$QT_SRC/qml"
            echo "[build-qt] Deployed mirobody_qt.app"
        else
            echo "[build-qt] macdeployqt or .app bundle not found; skipping deploy." >&2
        fi
    else
        echo "[build-qt] Linux: run against a Qt runtime (LD_LIBRARY_PATH to your Qt"
        echo "           lib dir; llama .so are already beside the exe)."
    fi
fi
