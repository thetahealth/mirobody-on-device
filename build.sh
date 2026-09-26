#!/bin/sh
# POSIX sh wrapper: configure (if needed) and build the core on Linux / macOS
# against system-installed libraries (see docs/BUILDING.md for the package
# list); vcpkg is Windows-only.
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

usage() {
    cat <<EOF
Usage: build.sh [arch] [mobile] [clean]      (tokens in any order)

  With no arguments: the development build for the host arch (into "build"):
  the core, the optional loopback HTTP front door, the debug CLIs and the tests.
  Pass help / -h / --help to show this help.

  Arch      amd64 (or x86_64), arm64, x86       default: host arch
            build.sh builds natively, so the arch must match the host.
  mobile    build the profile HarmonyOS ships: no HTTP front door (no
            libwebsockets). Libraries only -- no server executable, CLIs or tests.
            Into "build-mobile".
  clean     remove the build dir and reconfigure from scratch
  help / -h / --help   show this help

SQLite is the only database backend, so there is nothing to choose there.
Install system packages first (cmake, ninja, the libwebsockets / curl / ssl /
rapidjson / yaml-cpp / sqlite3 / image-codec -dev set, and catch2 for the
tests). With mobile, libwebsockets is not needed.
EOF
}

ARCH=""
CLEAN=""
MOBILE=""
for arg in "$@"; do
    case "$arg" in
        help|-h|--help|-\?) usage; exit 0 ;;
        clean)              CLEAN=1 ;;
        mobile)             MOBILE=1 ;;
        amd64|x86_64|x64)   ARCH=amd64 ;;
        arm64|aarch64)      ARCH=arm64 ;;
        x86|i386|i686)      ARCH=x86 ;;
        *) echo "Unknown argument: $arg" >&2; echo 'Run "build.sh -h" for usage.' >&2; exit 1 ;;
    esac
done

# Host arch (normalized to amd64|arm64|x86): the default target and the only
# one build.sh can produce.
case "$(uname -m)" in
    x86_64|amd64)  HOST_ARCH=amd64 ;;
    aarch64|arm64) HOST_ARCH=arm64 ;;
    i386|i686|x86) HOST_ARCH=x86 ;;
    *)             HOST_ARCH="$(uname -m)" ;;
esac

ARCH="${ARCH:-$HOST_ARCH}"
if [ "$ARCH" != "$HOST_ARCH" ]; then
    echo "build.sh builds natively (host is $HOST_ARCH); cross-building $ARCH is not supported." >&2
    exit 1
fi

# mobile gets its own build dir so it never clobbers the normal one, and its own
# CMake arg (see MIROBODY_MOBILE in CMakeLists.txt).
DIR="build"
PROFILE_ARG=""
if [ -n "$MOBILE" ]; then
    PROFILE_ARG="-DMIROBODY_MOBILE=ON"
    DIR="build-mobile"
fi

# Homebrew keeps libjpeg-turbo keg-only, so point CMake at it explicitly.
PREFIX_ARG=""
if command -v brew >/dev/null 2>&1; then
    BREW_PREFIX="$(brew --prefix)"
    PREFIX_ARG="-DCMAKE_PREFIX_PATH=$BREW_PREFIX/opt/jpeg-turbo;$BREW_PREFIX"
fi

BUILD_DIR="$PROJECT_DIR/$DIR"

# clean: wipe the build dir so the next run reconfigures from scratch.
if [ -n "$CLEAN" ] && [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
fi

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        $PREFIX_ARG \
        $PROFILE_ARG
fi

cmake --build "$BUILD_DIR" --config Release
