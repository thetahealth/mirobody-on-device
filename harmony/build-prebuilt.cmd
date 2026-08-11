@echo off
rem Cross-compile the C++ core's native dependencies for HarmonyOS / OpenHarmony
rem and lay them out under harmony\prebuilt\<ABI>\ so entry\src\main\cpp\CMakeLists.txt
rem (which passes -DCMAKE_PREFIX_PATH) and the repo-root CMakeLists.txt find_package()
rem calls resolve. The OHOS counterpart of android\build-prebuilt.cmd, same vcpkg
rem baseline and the same port set MINUS libwebsockets.
rem
rem No libwebsockets: HarmonyOS embeds the core through the C ABI (mirobody_chat),
rem not the HTTP front door, so the native target is configured with
rem the mobile profile (MIROBODY_MOBILE, auto-selected for OHOS) and never links
rem it. See CMakeLists.txt.
rem
rem vcpkg ships OHOS triplets but has taught no *port* about the platform, and the
rem pinned tool does not map CMAKE_SYSTEM_NAME=OHOS to its own toolchain. Both are
rem worked around by the overlay triplets in harmony\vcpkg-triplets\ -- read the
rem comments there before changing anything here.
rem
rem Re-running is idempotent: vcpkg skips already-built ports and the destination is
rem refreshed.
rem
rem prebuilt\ holds every prebuilt native artifact, not just these: build-llama.cmd
rem assembles the on-device engine into prebuilt\llama-sdk\<abi>\ beside them. This
rem script replaces only its own prebuilt\%ABI%, so the two never collide -- but a
rem hand `rmdir prebuilt` discards both.
rem
rem Usage:  build-prebuilt.cmd [abi]
rem   abi defaults to arm64-v8a; one of arm64-v8a^|x86_64^|armeabi-v7a.
rem Env overrides:
rem   VCPKG_ROOT      vcpkg checkout (default: a sibling of the repo)
rem   OHOS_SDK_ROOT   the SDK dir CONTAINING native\ (default: resolved from DevEco)
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "ABI=%~1"
if "%ABI%"=="" set "ABI=arm64-v8a"

rem READ from vcpkg.json's "builtin-baseline" rather than repeated here. The hash used to
rem be duplicated in four scripts (this, .sh, and both android ones) under a comment asking
rem the reader to keep them in sync by hand -- so bumping the pin in vcpkg.json and missing
rem one meant that platform silently kept building against the old port set.
rem tokens=4 delims=" splits `  "builtin-baseline": "<hash>",` into ["  "]["builtin-baseline"]
rem [": "][<hash>], so token 4 is the hash.
set "VCPKG_JSON=%~dp0..\vcpkg.json"
set "BASELINE="
for /f tokens^=4^ delims^=^" %%A in ('findstr /c:"builtin-baseline" "%VCPKG_JSON%"') do set "BASELINE=%%A"
if not defined BASELINE (
    echo no "builtin-baseline" in "%VCPKG_JSON%">&2
    exit /b 1
)

rem OHOS ABI -> overlay triplet (harmony\vcpkg-triplets\).
if /I "%ABI%"=="arm64-v8a"   ( set "TRIPLET=arm64-ohos" ) else ^
if /I "%ABI%"=="x86_64"      ( set "TRIPLET=x64-ohos"   ) else ^
if /I "%ABI%"=="armeabi-v7a" ( set "TRIPLET=arm-ohos"   ) else (
    echo abi must be arm64-v8a^|x86_64^|armeabi-v7a ^(got "%ABI%"^)>&2
    exit /b 2
)

rem Default the vcpkg checkout to a sibling of the repo so it is not committed.
if not defined VCPKG_ROOT for %%I in ("%~dp0..\..") do set "VCPKG_ROOT=%%~fI\vcpkg"

rem --- Resolve the OpenHarmony SDK ---------------------------------------------
rem OHOS_SDK_ROOT must name the directory CONTAINING native\ (vcpkg's
rem scripts\toolchains\ohos.cmake appends \native itself).
if not defined OHOS_SDK_ROOT (
    for %%D in (
        "%DEVECO_HOME%\sdk\default\openharmony"
        "%ProgramFiles%\Huawei\DevEco Studio\sdk\default\openharmony"
        "%LOCALAPPDATA%\OpenHarmony\Sdk\default\openharmony"
    ) do (
        if not defined OHOS_SDK_ROOT if exist "%%~D\native\build\cmake\ohos.toolchain.cmake" set "OHOS_SDK_ROOT=%%~D"
    )
)
if not defined OHOS_SDK_ROOT (
    echo OpenHarmony SDK not found. Set OHOS_SDK_ROOT to the directory containing native\.>&2
    exit /b 1
)
if not exist "%OHOS_SDK_ROOT%\native\build\cmake\ohos.toolchain.cmake" (
    echo No native\build\cmake\ohos.toolchain.cmake under "%OHOS_SDK_ROOT%".>&2
    exit /b 1
)

rem --- Space-free SDK path (mandatory) -----------------------------------------
rem OpenSSL's generated Makefile hands the compiler path and --sysroot to /bin/sh
rem unquoted, so a space anywhere in the SDK path breaks the build with
rem "/bin/sh: .../Huawei/DevEco: No such file or directory". Unlike the Android
rem script -- which mirrors the whole NDK with robocopy -- a directory junction is
rem enough here and costs nothing. Detect a space via substitution: !VAR: =! strips
rem spaces, so a differing result means VAR had one.
set "SDK_NOSP=!OHOS_SDK_ROOT: =!"
if not "!SDK_NOSP!"=="!OHOS_SDK_ROOT!" (
    set "VR_NOSP=!VCPKG_ROOT: =!"
    if not "!VR_NOSP!"=="!VCPKG_ROOT!" (
        echo SDK path contains a space ^["!OHOS_SDK_ROOT!"^] and so does VCPKG_ROOT;>&2
        echo point OHOS_SDK_ROOT at an SDK under a space-free path.>&2
        exit /b 1
    )
    set "SDK_LINK=!VCPKG_ROOT!\ohos-sdk"
    if not exist "!SDK_LINK!\native\build\cmake\ohos.toolchain.cmake" (
        echo SDK path has a space; linking a space-free alias: !SDK_LINK!
        if exist "!SDK_LINK!" rmdir "!SDK_LINK!" 2>nul
        mklink /J "!SDK_LINK!" "!OHOS_SDK_ROOT!" >nul
        if errorlevel 1 (
            echo could not create the junction ^(mklink /J^)>&2
            exit /b 1
        )
    )
    set "OHOS_SDK_ROOT=!SDK_LINK!"
)

echo ABI=%ABI%  triplet=!TRIPLET!
echo SDK=!OHOS_SDK_ROOT!
echo vcpkg=!VCPKG_ROOT!
rem Printed because it is no longer readable off this script -- it comes from vcpkg.json.
echo baseline=!BASELINE!

rem --- Bootstrap vcpkg at the pinned baseline -----------------------------------
if not exist "!VCPKG_ROOT!\.git" (
    echo Cloning vcpkg ...
    git clone https://github.com/microsoft/vcpkg "!VCPKG_ROOT!"
    if errorlevel 1 exit /b 1
)
git -C "!VCPKG_ROOT!" checkout %BASELINE%
if errorlevel 1 exit /b 1
if not exist "!VCPKG_ROOT!\vcpkg.exe" call "!VCPKG_ROOT!\bootstrap-vcpkg.bat" -disableMetrics

rem --- Build the core's deps for OHOS (classic mode) ----------------------------
rem --classic: this script runs inside the repo, whose vcpkg.json would otherwise
rem put vcpkg into manifest mode (which rejects port-name arguments).
rem No libwebsockets (see the header). openblas / libpq / libmysql / catch2 /
rem sentry-native / xlnt are desktop/test-only and omitted; the app uses SQLite.
set "PORTS=openssl curl[openssl] yaml-cpp hiredis rapidjson sqlite3 libjpeg-turbo libpng libwebp tiff zlib"
echo Installing: !PORTS!  (--triplet !TRIPLET!)
"!VCPKG_ROOT!\vcpkg.exe" install !PORTS! --classic --triplet !TRIPLET! ^
    --overlay-triplets="%~dp0vcpkg-triplets" ^
    --x-buildtrees-root "!VCPKG_ROOT!\buildtrees"
if errorlevel 1 exit /b 1

rem --- Lay out under harmony\prebuilt\<ABI> -------------------------------------
set "INSTALLED=!VCPKG_ROOT!\installed\!TRIPLET!"
if not exist "!INSTALLED!" (
    echo expected install tree missing: !INSTALLED!>&2
    exit /b 1
)
set "DEST=%~dp0prebuilt\%ABI%"
if exist "!DEST!" rmdir /s /q "!DEST!"
mkdir "!DEST!"
rem Copy debug\ too: vcpkg's *Targets.cmake reference both release (lib\) and debug
rem (debug\lib\) imported locations and CMake verifies both exist at find_package time.
for %%S in (include lib share debug) do (
    if exist "!INSTALLED!\%%S" (
        robocopy "!INSTALLED!\%%S" "!DEST!\%%S" /E /NFL /NDL /NJH /NJS >nul
        rem robocopy exits 0-7 on success, >=8 on failure.
        if errorlevel 8 (
            echo robocopy %%S failed>&2
            exit /b 1
        )
    )
)

echo.
echo Done. Prebuilt deps for %ABI% -^> !DEST!
echo Now build the app in DevEco, or the native target directly:
echo   cmake -B build-ohos -G Ninja ^^
echo     -DCMAKE_TOOLCHAIN_FILE=!OHOS_SDK_ROOT!/native/build/cmake/ohos.toolchain.cmake ^^
echo     -DOHOS_ARCH=%ABI% ^^
echo     -DCMAKE_FIND_ROOT_PATH=!DEST! -DCMAKE_PREFIX_PATH=!DEST! ^^
echo     -DMIROBODY_DATABASE_BACKEND=SQLITE ^^
echo     -DMIROBODY_BUILD_TOOLS=OFF -DMIROBODY_BUILD_TESTS=OFF -DMIROBODY_BUILD_SHARED=OFF
echo   cmake --build build-ohos --target mirobody_core
exit /b 0
