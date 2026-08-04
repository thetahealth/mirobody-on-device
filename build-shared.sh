#!/bin/sh
# POSIX sh: build libmirobody, the C-ABI shared library (libmirobody.so/.dylib)
# consumed by FFI hosts (Java, Go, C#, Node, ...). The Linux/macOS counterpart to
# build-shared.cmd; see bindings/README.md. Also builds the JNI shim
# (libmirobody_jni.so) when JAVA_HOME points at a JDK.
#
# Unlike the Windows script there is no /MT vs /MD juggling: that is MSVC-only,
# and on Linux/macOS the default vcpkg triplets already link the deps statically
# while glibc/libSystem is shared system-wide, so there is no bundled-CRT
# shadowing of the kind the JDK causes on Windows.
set -e

case "$1" in
    help|-h|--help|-\?)
        cat <<EOF
Usage: build-shared.sh [clean|-h]

  clean       reconfigure from scratch (keeps build-shared/vcpkg_installed)
  help / -h / --help   show this help

  Builds libmirobody (the C-ABI shared library) into build-shared/. Defaults to
  the SQLITE backend so the library is self-contained. Builds libmirobody_jni.so
  too when JAVA_HOME is set.

  If VCPKG_ROOT is set its toolchain + triplet are forwarded to CMake; otherwise
  system packages are used (same prerequisites as build.sh).

Environment variables:
  MIROBODY_DATABASE_BACKEND  SQL backend to link. Default: SQLITE.
  VCPKG_ROOT                 Optional vcpkg checkout; toolchain forwarded if set.
  VCPKG_TRIPLET              vcpkg triplet. Default: <arch>-<os> autodetected.
  JAVA_HOME                  If set, also build the JNI shim (libmirobody_jni).
EOF
        exit 0
        ;;
esac

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build-shared"

# clean: drop CMake's configuration but KEEP build-shared/vcpkg_installed, so the
# reconfigure below picks up changed options without the slow vcpkg dependency
# rebuild. (Only matters when VCPKG_ROOT is set; system-package builds keep nothing
# in the dir anyway.)
if [ "$1" = "clean" ] && [ -d "$BUILD_DIR" ]; then
    find "$BUILD_DIR" -mindepth 1 -maxdepth 1 ! -name vcpkg_installed -exec rm -rf {} +
fi

# Default to the self-contained SQLite backend; override to link another client.
MIROBODY_DATABASE_BACKEND="${MIROBODY_DATABASE_BACKEND:-SQLITE}"

# Base configure arguments.
set -- -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DMIROBODY_DATABASE_BACKEND="$MIROBODY_DATABASE_BACKEND" \
    -DMIROBODY_BUILD_SHARED=ON \
    -DMIROBODY_BUILD_TOOLS=OFF \
    -DMIROBODY_BUILD_TESTS=OFF

# Forward the vcpkg toolchain + triplet when VCPKG_ROOT is set; otherwise rely on
# system packages (same as build.sh).
if [ -n "$VCPKG_ROOT" ]; then
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
    set -- "$@" \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET="$VCPKG_TRIPLET"
fi

# Build the JNI shim too when a JDK is available (find_package(JNI) uses JAVA_HOME).
if [ -n "$JAVA_HOME" ]; then
    set -- "$@" -DMIROBODY_BUILD_JNI=ON
fi

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" "$@"
fi

cmake --build "$BUILD_DIR" --config Release --target mirobody_shared
if [ -n "$JAVA_HOME" ]; then
    cmake --build "$BUILD_DIR" --config Release --target mirobody_jni
fi
