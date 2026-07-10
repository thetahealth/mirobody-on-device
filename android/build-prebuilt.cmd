@echo off
rem Cross-compile the C++ server's native dependencies for Android and lay them out
rem under android\prebuilt\<ABI>\ so that app\build.gradle.kts (which passes
rem -DCMAKE_PREFIX_PATH=android/prebuilt/${ANDROID_ABI}) and the repo-root
rem CMakeLists.txt find_package() calls resolve. The Windows counterpart of
rem build-prebuilt.sh (same vcpkg baseline, triplets, and port set).
rem
rem Uses vcpkg's Android triplets (arm64-android, etc.), pinned to the same
rem builtin-baseline as vcpkg.json, to build exactly the libraries the server links:
rem openssl, curl (openssl backend), libwebsockets, yaml-cpp, hiredis, rapidjson,
rem sqlite3, and the image codecs the transcoder links (libjpeg-turbo, libpng,
rem libwebp, tiff, zlib). libpq / libmysql / openblas / catch2 / sentry-native /
rem xlnt are desktop/test-only (or optional on mobile) and omitted (mobile uses the
rem SQLite backend; tests are disabled on Android in CMakeLists.txt).
rem
rem Re-running is idempotent: vcpkg skips already-built ports and the destination is
rem refreshed.
rem
rem Usage:  build-prebuilt.cmd [abi]
rem   abi defaults to arm64-v8a; one of arm64-v8a^|armeabi-v7a^|x86_64^|x86.
rem Env overrides:
rem   VCPKG_ROOT         vcpkg checkout (default: a sibling of the repo)
rem   ANDROID_NDK_HOME   NDK to use (default: resolved from the SDK)
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "ABI=%~1"
if "%ABI%"=="" set "ABI=arm64-v8a"

rem Keep the pin in sync with vcpkg.json's "builtin-baseline".
set "BASELINE=d015e31e90838a4c9dfa3eed45979bc70d9357fc"
rem Must match the ndkVersion pinned in app\build.gradle.kts so the prebuilt deps and
rem the app share one libc++ (c++_shared) ABI; prefer this exact version, else newest.
set "PINNED_NDK=27.0.12077973"

rem Android ABI -> vcpkg triplet.
set "TRIPLET="
if /i "%ABI%"=="arm64-v8a"   set "TRIPLET=arm64-android"
if /i "%ABI%"=="armeabi-v7a" set "TRIPLET=arm-android"
if /i "%ABI%"=="x86_64"      set "TRIPLET=x64-android"
if /i "%ABI%"=="x86"         set "TRIPLET=x86-android"
if not defined TRIPLET (
    echo abi must be arm64-v8a^|armeabi-v7a^|x86_64^|x86 ^(got '%ABI%'^)>&2
    exit /b 2
)

rem Default the vcpkg checkout to a sibling of the repo so it is not committed.
if not defined VCPKG_ROOT (
    pushd "%~dp0..\.."
    set "VCPKG_ROOT=!CD!\vcpkg"
    popd
)

rem --- Resolve the NDK ----------------------------------------------------------
set "NDK_HOME=%ANDROID_NDK_HOME%"
if defined NDK_HOME if not exist "%NDK_HOME%" set "NDK_HOME="
if not defined NDK_HOME (
    set "SDK=%ANDROID_HOME%"
    if not defined SDK set "SDK=%LOCALAPPDATA%\Android\Sdk"
    set "NDK_DIR=!SDK!\ndk"
    if exist "!NDK_DIR!\%PINNED_NDK%\build\cmake\android.toolchain.cmake" (
        set "NDK_HOME=!NDK_DIR!\%PINNED_NDK%"
    ) else if exist "!NDK_DIR!" (
        rem Newest installed NDK (name-descending, matching the .sh/.ps1).
        for /f "delims=" %%D in ('dir /b /ad /o-n "!NDK_DIR!" 2^>nul') do (
            if not defined NDK_HOME set "NDK_HOME=!NDK_DIR!\%%D"
        )
    )
)
if not defined NDK_HOME goto :ndk_missing
if not exist "%NDK_HOME%\build\cmake\android.toolchain.cmake" goto :ndk_missing
rem Work around spaces in the NDK path. openssl's autoconf/make build invokes $(CC)
rem unquoted, so a profile like "C:\Users\Feng Xie" fails with
rem "/bin/sh: C:/Users/Feng: No such file or directory". The NDK's android.toolchain
rem .cmake resolves 8.3 short names and junctions back to the real spaced location,
rem so only a *physical* copy at a space-free path works: mirror the NDK once under
rem the (space-free) vcpkg root and build against the mirror.
rem Detect a space via string substitution (robust inside nested blocks, unlike a
rem piped `find`): !VAR: =! strips spaces; if the result differs, VAR had one.
set "NDK_NOSP=!NDK_HOME: =!"
if not "!NDK_NOSP!"=="!NDK_HOME!" (
    set "VR_NOSP=!VCPKG_ROOT: =!"
    if not "!VR_NOSP!"=="!VCPKG_ROOT!" (
        echo NDK path contains a space ^["!NDK_HOME!"^] and so does VCPKG_ROOT;>&2
        echo point ANDROID_NDK_HOME at an NDK under a space-free path.>&2
        exit /b 1
    )
    for %%I in ("!NDK_HOME!") do set "NDK_LEAF=%%~nxI"
    set "NDK_MIRROR=!VCPKG_ROOT!\android-ndk\!NDK_LEAF!"
    if not exist "!NDK_MIRROR!\build\cmake\android.toolchain.cmake" (
        echo NDK path has a space; mirroring to a space-free path: !NDK_MIRROR!
        robocopy "!NDK_HOME!" "!NDK_MIRROR!" /E /NFL /NDL /NJH /NJS /R:1 /W:1 >nul
        if errorlevel 8 (
            echo NDK mirror copy failed>&2
            exit /b 1
        )
    )
    set "NDK_HOME=!NDK_MIRROR!"
)
set "ANDROID_NDK_HOME=!NDK_HOME!"
echo ABI=%ABI%  triplet=%TRIPLET%
echo NDK=%NDK_HOME%
echo vcpkg=%VCPKG_ROOT%

rem --- Bootstrap vcpkg at the pinned baseline -----------------------------------
if not exist "%VCPKG_ROOT%\.git" (
    echo Cloning vcpkg ...
    git clone https://github.com/microsoft/vcpkg "%VCPKG_ROOT%"
    if errorlevel 1 exit /b 1
)
git -C "%VCPKG_ROOT%" checkout %BASELINE%
if errorlevel 1 exit /b 1
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
    call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics
    if errorlevel 1 exit /b 1
)

rem --- Build the server's deps for Android (classic mode) -----------------------
rem --classic: this script runs inside the repo, whose vcpkg.json would otherwise put
rem vcpkg into manifest mode (which rejects port-name arguments). Force classic mode.
echo Installing ports (--triplet %TRIPLET%) ...
"%VCPKG_ROOT%\vcpkg.exe" install openssl "curl[openssl]" libwebsockets yaml-cpp hiredis rapidjson sqlite3 libjpeg-turbo libpng libwebp tiff zlib --classic --triplet %TRIPLET% --x-buildtrees-root "%VCPKG_ROOT%\buildtrees"
if errorlevel 1 exit /b 1

rem --- Lay out under android\prebuilt\<ABI> -------------------------------------
set "INSTALLED=%VCPKG_ROOT%\installed\%TRIPLET%"
if not exist "%INSTALLED%" (
    echo expected install tree missing: %INSTALLED%>&2
    exit /b 1
)
set "DEST=%~dp0prebuilt\%ABI%"
if exist "%DEST%" rmdir /s /q "%DEST%"
mkdir "%DEST%"
rem Copy debug\ too: vcpkg's *Targets.cmake reference both release (lib\) and debug
rem (debug\lib\) imported locations and CMake verifies both exist at find_package time.
for %%S in (include lib share debug) do (
    if exist "%INSTALLED%\%%S" (
        robocopy "%INSTALLED%\%%S" "%DEST%\%%S" /E /NFL /NDL /NJH /NJS >nul
        rem robocopy exits 0-7 on success, >=8 on failure.
        if errorlevel 8 (
            echo robocopy %%S failed>&2
            exit /b 1
        )
    )
)

echo.
echo Done. Prebuilt deps for %ABI% -^> %DEST%
echo Now build the app (Android Studio) or the native target directly:
echo   cmake -B build-android -G Ninja
echo     -DCMAKE_TOOLCHAIN_FILE=%NDK_HOME%\build\cmake\android.toolchain.cmake
echo     -DANDROID_ABI=%ABI% -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared
echo     -DCMAKE_FIND_ROOT_PATH=%DEST% -DCMAKE_PREFIX_PATH=%DEST% -DMIROBODY_DATABASE_BACKEND=SQLITE
echo     -DMIROBODY_BUILD_TOOLS=OFF -DMIROBODY_BUILD_TESTS=OFF
echo   cmake --build build-android --target mirobody
exit /b 0

:ndk_missing
echo Android NDK not found. Set ANDROID_NDK_HOME (or ANDROID_HOME).>&2
exit /b 1
