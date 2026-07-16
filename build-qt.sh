#!/usr/bin/env bash
# build-qt.sh -- build the standalone Qt Quick desktop client (qt/) on Linux/macOS.
#
# qt/CMakeLists.txt has no project() call (it is designed to be add_subdirectory'd
# from the top-level build, and links NOTHING from mirobody_core), so this script
# generates a tiny wrapper CMakeLists and points CMake at that. The exe lands in
# build-qt/app/ NEXT TO its generated Mirobody/ QML module dir (the "Mirobody"
# module is loaded from disk beside the exe, NOT embedded), so it must not be
# relocated away from that directory.
#
# Usage: ./build-qt.sh [clean] [deploy]   (tokens in any order)
#   clean    wipe the CMake cache and reconfigure from scratch
#   deploy   macOS: run macdeployqt on the .app. Linux: prints guidance
#            (use your distro's Qt or a tool like linuxdeployqt).
#   -h / --help   show this help
#
# Env overrides:
#   QT_PREFIX   Qt kit dir (CMAKE_PREFIX_PATH). If unset, relies on Qt6 being
#               discoverable by CMake (system install / qtchooser / PATH).
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QT_SRC="$PROJECT_DIR/qt"
BUILD_DIR="$PROJECT_DIR/build-qt"
WRAP_DIR="$PROJECT_DIR/build-qt-wrap"
CACHE_DIR="$BUILD_DIR"
EXE="$BUILD_DIR/app/mirobody_qt"

CLEAN=""
DEPLOY=""
for tok in "$@"; do
    case "$tok" in
        clean)  CLEAN=1 ;;
        deploy) DEPLOY=1 ;;
        -h|--help)
            sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "[build-qt] Unknown token: $tok" >&2; exit 2 ;;
    esac
done

# --- clean + generate the wrapper CMakeLists (add_subdirectory the qt/ subtree) ---
[ -n "$CLEAN" ] && rm -rf "$CACHE_DIR"
mkdir -p "$WRAP_DIR"
cat > "$WRAP_DIR/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.19)
project(mirobody_qt_standalone LANGUAGES C CXX)
add_subdirectory("$QT_SRC" app)
EOF

# --- configure + build. The exe lands in build-qt/app/ NEXT TO its generated
#     Mirobody/ QML module dir (loaded from disk), so we do NOT relocate it. ---
CMAKE_ARGS=(
    -S "$WRAP_DIR" -B "$CACHE_DIR"
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
)
[ -n "${QT_PREFIX:-}" ] && CMAKE_ARGS+=(-DCMAKE_PREFIX_PATH="$QT_PREFIX")

if [ ! -f "$CACHE_DIR/build.ninja" ]; then
    cmake "${CMAKE_ARGS[@]}"
fi
cmake --build "$CACHE_DIR" --target mirobody_qt

echo "[build-qt] Built: $EXE"

# --- optional deployment ---
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
        echo "[build-qt] Linux has no windeployqt/macdeployqt. Run against a Qt runtime"
        echo "           (LD_LIBRARY_PATH to your Qt lib dir, or package with linuxdeployqt)."
    fi
fi
