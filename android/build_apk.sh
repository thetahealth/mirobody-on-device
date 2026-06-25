#!/usr/bin/env bash
# Build the Mirobody Android APK (UI client + embedded C++ server).
#
# Steps: ensure the native deps are cross-compiled for the target ABI, resolve a
# Gradle (./gradlew -> gradle on PATH -> download the pinned distribution), then
# assemble the APK. The embedded server's libmirobody.so is compiled by Gradle's
# externalNativeBuild only when android/prebuilt/<ABI>/ exists.
#
# Usage:  ./build_apk.sh [debug|release] [abi] [phone|watch]
#   abi defaults to arm64-v8a (the ABI wired in app/build.gradle.kts).
#   flavor defaults to phone; watch targets small AOSP wearables (~410x502).
set -euo pipefail

cd "$(dirname "$0")"
BUILD_TYPE="${1:-debug}"
ABI="${2:-arm64-v8a}"
FLAVOR="${3:-phone}"
case "$BUILD_TYPE" in
  debug|release) ;;
  *) echo "build type must be 'debug' or 'release' (got '$BUILD_TYPE')" >&2; exit 2 ;;
esac
case "$FLAVOR" in
  phone|watch) ;;
  *) echo "flavor must be 'phone' or 'watch' (got '$FLAVOR')" >&2; exit 2 ;;
esac
# Capitalize for the Gradle task name (assemblePhoneDebug / assembleWatchRelease).
cap() { printf '%s' "$(printf '%s' "${1:0:1}" | tr '[:lower:]' '[:upper:]')${1:1}"; }
TASK="assemble$(cap "$FLAVOR")$(cap "$BUILD_TYPE")"

# --- 1. Ensure prebuilt native deps -------------------------------------------
if [ ! -d "prebuilt/$ABI" ]; then
  echo "==> prebuilt/$ABI missing; cross-compiling native deps"
  if command -v pwsh >/dev/null 2>&1; then
    pwsh -File ./build-prebuilt.ps1 -Abi "$ABI"
  else
    echo "ERROR: prebuilt/$ABI not found and PowerShell (pwsh) is unavailable." >&2
    echo "Produce the deps first (see build-prebuilt.ps1), then re-run." >&2
    exit 1
  fi
fi

# --- 2. Resolve a Gradle launcher ---------------------------------------------
if [ -x ./gradlew ]; then
  GRADLE=(./gradlew)
elif command -v gradle >/dev/null 2>&1; then
  GRADLE=(gradle)
else
  # Download the distribution pinned in gradle/wrapper/gradle-wrapper.properties.
  URL="$(sed -n 's/^distributionUrl=//p' gradle/wrapper/gradle-wrapper.properties | sed 's/\\:/:/g')"
  VER="$(printf '%s' "$URL" | grep -oE 'gradle-[0-9.]+' | head -1 | sed 's/gradle-//')"
  CACHE="${GRADLE_USER_HOME:-$HOME/.gradle}/mirobody-dist"
  BIN="$CACHE/gradle-$VER/bin/gradle"
  if [ ! -x "$BIN" ]; then
    echo "==> No gradlew/gradle found; downloading Gradle $VER"
    mkdir -p "$CACHE"
    curl -fL "$URL" -o "$CACHE/gradle-$VER.zip"
    unzip -q -o "$CACHE/gradle-$VER.zip" -d "$CACHE"
  fi
  GRADLE=("$BIN")
fi

# --- 3. Build -----------------------------------------------------------------
echo "==> ${GRADLE[*]} :app:$TASK (ABI=$ABI)"
"${GRADLE[@]}" ":app:$TASK" --no-daemon

# --- 4. Report ----------------------------------------------------------------
# Product flavours nest the output under apk/<flavor>/<type>/.
OUT="app/build/outputs/apk/$FLAVOR/$BUILD_TYPE"
APK="$(find "$OUT" -name '*.apk' 2>/dev/null | head -1 || true)"
if [ -n "$APK" ]; then
  echo "APK: $(cd "$(dirname "$APK")" && pwd)/$(basename "$APK")"
else
  echo "Build finished but no APK found under $OUT" >&2
  exit 1
fi
