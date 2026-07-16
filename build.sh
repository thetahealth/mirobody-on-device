#!/bin/sh
# POSIX sh wrapper: pick arch + backend, then configure (if needed) and build.
# Linux / macOS build against system-installed libraries (see the README
# "Building - Linux / WSL / macOS" for the package list); vcpkg is Windows-only.
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

usage() {
    cat <<EOF
Usage: build.sh [arch] [backend] [clean]      (tokens in any order)

  With no arguments: build the host arch with the POSTGRESQL default
  (into "build"). Pass help / -h / --help to show this help.

  Arch      amd64 (or x86_64), arm64, x86       default: host arch
            build.sh builds natively, so the arch must match the host.
  Backend   pg / postgresql, legacy / pg_legacy, mysql, sqlite, ck / clickhouse, duckdb
            (omit for the POSTGRESQL default)
  clean     remove the build dir and reconfigure from scratch
  help / -h / --help   show this help

The build dir is build[-<arch>][-<backend>]: the arch suffix is omitted for the
host arch, the backend suffix for the POSTGRESQL default. So the plain
host+postgresql build is just "build"; "build.sh legacy" -> build-legacy, and each
combo gets its own dir so they can coexist.

Arch and backend come only from the command line (no environment variables).

Install system packages first (cmake, ninja, the libwebsockets / curl / ssl /
rapidjson / yaml-cpp / hiredis -dev set, plus the chosen backend's -dev package).
See the README "Building - Linux / WSL / macOS" section.
EOF
}

# --- Parse args: arch and/or backend selectors in any order, plus `clean`. ---
ARCH=""
DB_BACKEND=""
CLEAN=""
for arg in "$@"; do
    case "$arg" in
        help|-h|--help|-\?) usage; exit 0 ;;
        clean)              CLEAN=1 ;;
        amd64|x86_64|x64)   ARCH=amd64 ;;
        arm64|aarch64)      ARCH=arm64 ;;
        x86|i386|i686)      ARCH=x86 ;;
        pg|postgresql)      DB_BACKEND=POSTGRESQL ;;
        legacy|pg_legacy)   DB_BACKEND=POSTGRESQL_LEGACY ;;
        mysql)              DB_BACKEND=MYSQL ;;
        sqlite)             DB_BACKEND=SQLITE ;;
        duckdb)             DB_BACKEND=DUCKDB ;;
        ck|clickhouse)      DB_BACKEND=CLICKHOUSE ;;
        *) echo "Unknown argument: $arg" >&2; echo 'Run "build.sh -h" for usage.' >&2; exit 1 ;;
    esac
done

# Host arch (normalized to amd64|arm64|x86), the default target and the yardstick
# for whether the build dir needs an arch suffix.
case "$(uname -m)" in
    x86_64|amd64)  HOST_ARCH=amd64 ;;
    aarch64|arm64) HOST_ARCH=arm64 ;;
    i386|i686|x86) HOST_ARCH=x86 ;;
    *)             HOST_ARCH="$(uname -m)" ;;
esac

# Resolve target arch (CLI token, else host). build.sh builds natively only.
ARCH="${ARCH:-$HOST_ARCH}"
if [ "$ARCH" != "$HOST_ARCH" ]; then
    echo "build.sh builds natively (host is $HOST_ARCH); cross-building $ARCH is not supported." >&2
    exit 1
fi

# Resolve backend (CLI token, else the POSTGRESQL default) and derive the short
# dir tag (the default POSTGRESQL has none, so its dir is plain "build").
DB_BACKEND="${DB_BACKEND:-POSTGRESQL}"
case "$DB_BACKEND" in
    POSTGRESQL_LEGACY) DB_TAG=legacy ;;
    MYSQL)      DB_TAG=mysql ;;
    SQLITE)     DB_TAG=sqlite ;;
    DUCKDB)     DB_TAG=duckdb ;;
    CLICKHOUSE) DB_TAG=ck ;;
    *)          DB_TAG="" ;;
esac

# Build dir: build[-<tag>]. The arch suffix is dropped for the host arch (the
# only arch build.sh targets), the backend suffix for the POSTGRESQL default.
DIR="build"
[ -n "$DB_TAG" ] && DIR="${DIR}-${DB_TAG}"
BUILD_DIR="$PROJECT_DIR/$DIR"

# clean: wipe the build dir so the next run reconfigures from scratch.
if [ -n "$CLEAN" ] && [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
fi

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DMIROBODY_DATABASE_BACKEND="$DB_BACKEND"
fi

cmake --build "$BUILD_DIR" --config Release
