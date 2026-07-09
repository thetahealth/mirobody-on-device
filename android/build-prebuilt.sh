#!/usr/bin/env bash
# Cross-compile the C++ server's native dependencies for Android and lay them out
# under android/prebuilt/<ABI>/ so that app/build.gradle.kts (which passes
# -DCMAKE_PREFIX_PATH=android/prebuilt/${ANDROID_ABI}) and the repo-root
# CMakeLists.txt find_package() calls resolve. The macOS/Linux counterpart of
# build-prebuilt.cmd (same vcpkg baseline, triplets, and port set).
#
# Uses vcpkg's Android triplets (arm64-android, etc.), pinned to the same
# builtin-baseline as vcpkg.json, to build exactly the libraries the server links:
# openssl, curl (openssl backend), libwebsockets, yaml-cpp, hiredis, rapidjson,
# sqlite3, and the image codecs the transcoder links (libjpeg-turbo, libpng,
# libwebp, tiff, zlib). libpq / libmysql / openblas / catch2 / sentry-native /
# xlnt are desktop/test-only (or optional on mobile) and omitted (mobile uses the
# SQLite backend; tests are disabled on Android in CMakeLists.txt).
#
# Re-running is idempotent: vcpkg skips already-built ports and the destination is
# refreshed.
#
# Usage:  ./build-prebuilt.sh [abi]
#   abi defaults to arm64-v8a; one of arm64-v8a|armeabi-v7a|x86_64|x86.
# Env overrides:
#   VCPKG_ROOT           vcpkg checkout (default: a sibling of the repo)
#   ANDROID_NDK_HOME     NDK to use (default: resolved from the SDK)
set -euo pipefail

cd "$(dirname "$0")"
SCRIPT_DIR="$(pwd)"
REPO_ROOT="$(cd .. && pwd)"

ABI="${1:-arm64-v8a}"

# Keep the pin in sync with vcpkg.json's "builtin-baseline".
BASELINE="d015e31e90838a4c9dfa3eed45979bc70d9357fc"
# Must match the ndkVersion pinned in app/build.gradle.kts so the prebuilt deps and
# the app share one libc++ (c++_shared) ABI; prefer this exact version, else newest.
PINNED_NDK="27.0.12077973"

# Android ABI -> vcpkg triplet.
case "$ABI" in
  arm64-v8a)   TRIPLET="arm64-android" ;;
  armeabi-v7a) TRIPLET="arm-android" ;;
  x86_64)      TRIPLET="x64-android" ;;
  x86)         TRIPLET="x86-android" ;;
  *) echo "abi must be arm64-v8a|armeabi-v7a|x86_64|x86 (got '$ABI')" >&2; exit 2 ;;
esac

# Default the vcpkg checkout to a sibling of the repo so it is not committed.
VCPKG_ROOT="${VCPKG_ROOT:-$(cd .. && cd .. && pwd)/vcpkg}"

# --- Resolve the NDK ----------------------------------------------------------
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
    # Newest installed NDK (version-sorted).
    NDK_HOME="$(ls -1d "$NDK_DIR"/*/ 2>/dev/null | sort -Vr | head -1 | sed 's:/*$::')"
  fi
fi
if [ -z "$NDK_HOME" ] || [ ! -f "$NDK_HOME/build/cmake/android.toolchain.cmake" ]; then
  echo "Android NDK not found. Set ANDROID_NDK_HOME (or ANDROID_HOME)." >&2
  exit 1
fi
export ANDROID_NDK_HOME="$NDK_HOME"
echo "ABI=$ABI  triplet=$TRIPLET"
echo "NDK=$NDK_HOME"
echo "vcpkg=$VCPKG_ROOT"

# --- Bootstrap vcpkg at the pinned baseline -----------------------------------
if [ ! -d "$VCPKG_ROOT/.git" ]; then
  echo "Cloning vcpkg ..."
  git clone https://github.com/microsoft/vcpkg "$VCPKG_ROOT"
fi
git -C "$VCPKG_ROOT" checkout "$BASELINE"
if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

# --- Build the server's deps for Android (classic mode) -----------------------
# --classic: this script runs inside the repo, whose vcpkg.json would otherwise put
# vcpkg into manifest mode (which rejects port-name arguments). Force classic mode.
PORTS=(openssl "curl[openssl]" libwebsockets yaml-cpp hiredis rapidjson sqlite3 libjpeg-turbo libpng libwebp tiff zlib)
echo "Installing: ${PORTS[*]}  (--triplet $TRIPLET)"
"$VCPKG_ROOT/vcpkg" install "${PORTS[@]}" --classic --triplet "$TRIPLET" \
  --x-buildtrees-root "$VCPKG_ROOT/buildtrees"

# --- Lay out under android/prebuilt/<ABI> -------------------------------------
INSTALLED="$VCPKG_ROOT/installed/$TRIPLET"
[ -d "$INSTALLED" ] || { echo "expected install tree missing: $INSTALLED" >&2; exit 1; }
DEST="$SCRIPT_DIR/prebuilt/$ABI"
rm -rf "$DEST"
mkdir -p "$DEST"
# Copy debug/ too: vcpkg's *Targets.cmake reference both release (lib/) and debug
# (debug/lib/) imported locations and CMake verifies both exist at find_package time.
for sub in include lib share debug; do
  if [ -d "$INSTALLED/$sub" ]; then
    cp -R "$INSTALLED/$sub" "$DEST/$sub"
  fi
done

echo ""
echo "Done. Prebuilt deps for $ABI -> $DEST"
echo "Now build the app (Android Studio) or the native target directly:"
echo "  cmake -B build-android -G Ninja \\"
echo "    -DCMAKE_TOOLCHAIN_FILE=$NDK_HOME/build/cmake/android.toolchain.cmake \\"
echo "    -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared \\"
echo "    -DCMAKE_FIND_ROOT_PATH=$DEST -DCMAKE_PREFIX_PATH=$DEST -DMIROBODY_DATABASE_BACKEND=SQLITE \\"
echo "    -DMIROBODY_BUILD_TOOLS=OFF -DMIROBODY_BUILD_TESTS=OFF"
echo "  cmake --build build-android --target mirobody"
