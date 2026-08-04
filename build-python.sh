#!/bin/sh
# POSIX sh: build the Python wheel (mirobody._mirobody) via scikit-build-core +
# pybind11. The Linux/macOS counterpart to build-python.cmd; see python/README.md.
set -e

case "$1" in
    help|-h|--help|-\?)
        cat <<EOF
Usage: build-python.sh [clean|-h]

  clean       reconfigure from scratch (keeps build-python/vcpkg_installed)
  help / -h / --help   show this help

  Builds the mirobody Python wheel into dist/ using the vcpkg toolchain (same
  deps as build.sh). Install a C++ compiler, CMake >= 3.19, and the vcpkg deps
  first; point VCPKG_ROOT at your vcpkg checkout.

Environment variables:
  VCPKG_ROOT      vcpkg checkout (required). Its toolchain + triplet are
                  forwarded to CMake.
  VCPKG_TRIPLET   vcpkg triplet. Default: <arch>-<os> autodetected
                  (e.g. x64-linux, arm64-osx).
EOF
        exit 0
        ;;
esac

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -z "$VCPKG_ROOT" ]; then
    echo "build-python.sh: VCPKG_ROOT is not set; point it at your vcpkg checkout." >&2
    exit 1
fi

# Detect the vcpkg triplet (<arch>-<os>) unless the caller pinned one.
if [ -z "$VCPKG_TRIPLET" ]; then
    case "$(uname -m)" in
        arm64|aarch64) _ARCH=arm64 ;;
        x86_64|amd64)  _ARCH=x64 ;;
        *)             _ARCH=x64 ;;
    esac
    case "$(uname -s)" in
        Darwin) _OS=osx ;;
        *)      _OS=linux ;;
    esac
    VCPKG_TRIPLET="$_ARCH-$_OS"
fi

# Forward the vcpkg toolchain + triplet AND a hard Release build type to CMake.
# SKBUILD_CMAKE_ARGS is semicolon-separated and appended last so it wins.
export SKBUILD_CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake;-DVCPKG_TARGET_TRIPLET=$VCPKG_TRIPLET;-DCMAKE_BUILD_TYPE=Release"

# Reusable build tree (build-* is gitignored), so reconfigures are incremental.
export SKBUILD_BUILD_DIR="$PROJECT_DIR/build-python"

# clean: drop CMake's configuration but KEEP build-python/vcpkg_installed, so the
# wheel build below reconfigures (picking up changed options) without the slow
# vcpkg dependency rebuild a full wipe forces.
if [ "$1" = "clean" ] && [ -d "$SKBUILD_BUILD_DIR" ]; then
    find "$SKBUILD_BUILD_DIR" -mindepth 1 -maxdepth 1 ! -name vcpkg_installed -exec rm -rf {} +
fi

python3 -m pip wheel "$PROJECT_DIR/." --wheel-dir "$PROJECT_DIR/dist"
