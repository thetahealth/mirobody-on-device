@echo off
rem Build the Android app (APK) from the CLI on Windows, setting up the environment the
rem native build needs. The Kotlin/Compose client builds with plain Gradle, but embedding
rem the in-process C++ server (libmirobody.so, built from the repo-root CMakeLists via the
rem NDK) needs three things that the default Windows environment gets wrong -- all traceable
rem to a space in the NDK path and to machine-wide MSVC env vars:
rem
rem   1. Space-free NDK. If the SDK NDK lives under a spaced profile ("C:\Users\Feng Xie"),
rem      the NDK toolchain invokes the compiler via an 8.3 short name -- clang++.exe becomes
rem      CLANG_~1.EXE, which drops the "++", so clang links in C-driver mode and libc++ /
rem      libc++abi go unresolved. build-prebuilt.cmd mirrors the NDK to a space-free path;
rem      we point Gradle at that mirror via MIROBODY_NDK_PATH (see app/build.gradle.kts).
rem   2. No MSVC/Windows-SDK include+lib leakage. clang honours INCLUDE / CPLUS_INCLUDE_PATH /
rem      C_INCLUDE_PATH, which are set machine-wide here for desktop C++; they pull the Windows
rem      ucrt headers into the Android cross-compile ("vcruntime.h not found"). Scrub them.
rem   3. Gradle 9.4.1 + Android Studio's JBR. The PATH `gradle` is 8.2.1 (too old); the
rem      daemon-jvm criteria want JetBrains JDK 21.
rem
rem Usage:  build-app.cmd [gradle-task]      (default task: :app:assembleDebug)
rem Env overrides: VCPKG_ROOT, JAVA_HOME, MIROBODY_NDK_PATH, GRADLE_BIN.
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "PINNED_NDK=27.0.12077973"

rem --- vcpkg root (the NDK mirror build-prebuilt.cmd creates lives here) ---------
if not defined VCPKG_ROOT (
    pushd "%~dp0..\.."
    set "VCPKG_ROOT=!CD!\vcpkg"
    popd
)

rem --- Space-free NDK: prefer the mirror; else fall back to the SDK NDK ----------
if not defined MIROBODY_NDK_PATH (
    set "NDK_MIRROR=!VCPKG_ROOT!\android-ndk\%PINNED_NDK%"
    if exist "!NDK_MIRROR!\build\cmake\android.toolchain.cmake" set "MIROBODY_NDK_PATH=!NDK_MIRROR!"
)
if not defined MIROBODY_NDK_PATH (
    echo NOTE: no space-free NDK mirror found; run build-prebuilt.cmd first if the native
    echo       build fails to link libc++ ^(undefined std:: / __cxa_* symbols^).
)

rem --- JAVA_HOME: Android Studio's JBR (JetBrains JDK 21) ------------------------
if exist "%ProgramFiles%\Android\Android Studio\jbr\bin\java.exe" (
    set "JAVA_HOME=%ProgramFiles%\Android\Android Studio\jbr"
)

rem --- Keep MSVC / Windows-SDK headers+libs out of the Android cross-compile -----
set "INCLUDE="
set "LIB="
set "CPLUS_INCLUDE_PATH="
set "C_INCLUDE_PATH="

rem --- Locate Gradle 9.4.1 (the wrapper pins it; no gradlew is checked in) -------
if not defined GRADLE_BIN (
    if exist "%USERPROFILE%\.gradle\mirobody-dist\gradle-9.4.1\bin\gradle.bat" (
        set "GRADLE_BIN=%USERPROFILE%\.gradle\mirobody-dist\gradle-9.4.1\bin\gradle.bat"
    )
)
if not defined GRADLE_BIN (
    for /d %%D in ("%USERPROFILE%\.gradle\wrapper\dists\gradle-9.4.1-bin\*") do (
        if exist "%%D\gradle-9.4.1\bin\gradle.bat" set "GRADLE_BIN=%%D\gradle-9.4.1\bin\gradle.bat"
    )
)
if not defined GRADLE_BIN (
    echo Gradle 9.4.1 not found. Open android\ in Android Studio once ^(it fetches 9.4.1^),>&2
    echo or set GRADLE_BIN to a 9.4.1 gradle.bat.>&2
    exit /b 1
)

set "TASK=%~1"
if "%TASK%"=="" set "TASK=:app:assembleDebug"

echo NDK=%MIROBODY_NDK_PATH%
echo JAVA_HOME=%JAVA_HOME%
echo gradle=%GRADLE_BIN%
echo task=%TASK%

rem --no-daemon so the CMake subprocess inherits this cleaned environment rather than a
rem stale daemon started with the MSVC env still present.
"%GRADLE_BIN%" -p "%~dp0." %TASK% --no-daemon --console=plain
exit /b %ERRORLEVEL%
