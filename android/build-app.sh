#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Run any Gradle task against the Android app with the environment the native build
# needs. The macOS/Linux counterpart of build-app.cmd.
#
#   ./build-app.sh [gradle-task]              (default: :app:assembleDebug)
#
# Deliberately thinner than build-app.cmd, because most of what that script exists to
# fix is a Windows problem and has no equivalent here:
#
#   - the space-free NDK mirror dodges 8.3 short names turning clang++.exe into
#     CLANG_~1.EXE, which drops the "++" and links in C-driver mode. Nothing on macOS or
#     Linux mangles a path, so a spaced one is merely a path.
#   - scrubbing INCLUDE / LIB / CPLUS_INCLUDE_PATH keeps machine-wide MSVC and
#     Windows-SDK headers out of the cross-compile. There are none to leak here.
#
# What DOES carry over is the toolchain resolution: a Gradle new enough for the build
# (the wrapper pins it, and no gradlew is checked in) and a JDK 21. Both are found the
# same way build-apk.sh finds them, on purpose -- two scripts that disagree about which
# Gradle is "the" one produce two different build outputs from one source tree.
#
# For the ordinary "build me an APK" errand, use build-apk.sh: it also cross-compiles the
# native deps first and reports where the APK landed. This script is for everything else
# -- :app:testPhoneDebugUnitTest, :app:lint, :app:installPhoneDebug, a --scan run.
#
# Env overrides: JAVA_HOME, GRADLE_BIN, ANDROID_NDK_HOME.
# ---------------------------------------------------------------------------
set -euo pipefail

cd "$(dirname "$0")"
TASK="${1:-:app:assembleDebug}"

# --- JAVA_HOME: a JDK 21, which the daemon-jvm criteria require -----------------
# Android Studio's bundled JetBrains Runtime is the one most likely to be present and
# the one Studio itself builds with, so an inconsistency between IDE and CLI cannot come
# from here. An explicit JAVA_HOME always wins.
if [ -z "${JAVA_HOME:-}" ]; then
    for d in \
      "/Applications/Android Studio.app/Contents/jbr/Contents/Home" \
      "$HOME/Applications/Android Studio.app/Contents/jbr/Contents/Home" \
      "/opt/android-studio/jbr" \
      "/usr/local/android-studio/jbr"
    do
        if [ -x "$d/bin/java" ]; then JAVA_HOME="$d"; break; fi
    done
fi
[ -n "${JAVA_HOME:-}" ] && export JAVA_HOME

# --- NDK ------------------------------------------------------------------------
# Only passed along when already set: Gradle resolves the NDK itself from the SDK, and
# overriding that with a guess would be worse than leaving it alone. MIROBODY_NDK_PATH is
# what app/build.gradle.kts reads.
if [ -n "${ANDROID_NDK_HOME:-}" ] && [ -z "${MIROBODY_NDK_PATH:-}" ]; then
    export MIROBODY_NDK_PATH="$ANDROID_NDK_HOME"
fi

# --- Gradle ---------------------------------------------------------------------
# Same order as build-apk.sh: ./gradlew, then one on PATH, then the pinned distribution
# downloaded into a cache of our own.
if [ -n "${GRADLE_BIN:-}" ]; then
    GRADLE=("$GRADLE_BIN")
elif [ -x ./gradlew ]; then
    GRADLE=(./gradlew)
elif command -v gradle >/dev/null 2>&1; then
    GRADLE=(gradle)
else
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

echo "JAVA_HOME=${JAVA_HOME:-<default>}"
echo "NDK=${MIROBODY_NDK_PATH:-<gradle default>}"
echo "gradle=${GRADLE[*]}"
echo "task=$TASK"

# --no-daemon so the CMake subprocess inherits THIS environment rather than a daemon's,
# started earlier with different variables and still running.
"${GRADLE[@]}" -p . "$TASK" --no-daemon --console=plain
