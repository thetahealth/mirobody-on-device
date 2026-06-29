@echo off
setlocal EnableExtensions

set "PROJECT_DIR=%~dp0"

:: --- Parse args: arch and/or backend selectors in any order, plus `clean`. ---
:: Arch tokens set _ARCH; backend tokens set _DB_BACKEND (the canonical CMake
:: value); `clean` sets _CLEAN. Unknown tokens are an error. Selection is purely
:: from the command line -- no environment variables influence arch or backend.
set "_ARCH="
set "_DB_BACKEND="
set "_CLEAN="

:parse
if "%~1"=="" goto :parsed
set "_T=%~1"
if /I "%_T%"=="/?"      goto :usage
if /I "%_T%"=="-h"      goto :usage
if /I "%_T%"=="--help"  goto :usage
if /I "%_T%"=="help"    goto :usage
if /I "%_T%"=="clean"   ( set "_CLEAN=1"          & shift & goto :parse )
if /I "%_T%"=="amd64"   ( set "_ARCH=amd64"       & shift & goto :parse )
if /I "%_T%"=="x86_64"  ( set "_ARCH=amd64"       & shift & goto :parse )
if /I "%_T%"=="x64"     ( set "_ARCH=amd64"       & shift & goto :parse )
if /I "%_T%"=="arm64"   ( set "_ARCH=arm64"       & shift & goto :parse )
if /I "%_T%"=="x86"     ( set "_ARCH=x86"         & shift & goto :parse )
if /I "%_T%"=="pg"          ( set "_DB_BACKEND=POSTGRESQL"        & shift & goto :parse )
if /I "%_T%"=="postgresql"  ( set "_DB_BACKEND=POSTGRESQL"        & shift & goto :parse )
if /I "%_T%"=="legacy"      ( set "_DB_BACKEND=POSTGRESQL_LEGACY" & shift & goto :parse )
if /I "%_T%"=="pg_legacy"   ( set "_DB_BACKEND=POSTGRESQL_LEGACY" & shift & goto :parse )
if /I "%_T%"=="mysql"       ( set "_DB_BACKEND=MYSQL"             & shift & goto :parse )
if /I "%_T%"=="sqlite"      ( set "_DB_BACKEND=SQLITE"            & shift & goto :parse )
if /I "%_T%"=="duckdb"      ( set "_DB_BACKEND=DUCKDB"            & shift & goto :parse )
if /I "%_T%"=="ck"          ( set "_DB_BACKEND=CLICKHOUSE"        & shift & goto :parse )
if /I "%_T%"=="clickhouse"  ( set "_DB_BACKEND=CLICKHOUSE"        & shift & goto :parse )
echo Unknown argument: %_T%
echo Run "build.cmd -h" for usage.
exit /b 1
:parsed

:: Bring up the toolchain (VS_DIR/NINJA defaults, vcvars, VCPKG_ROOT) and resolve
:: the arch. The helper reads a preset VS_ARCH (our CLI token) or defaults to the
:: host; it sets HOST_ARCH and VCPKG_ARCH (the vcpkg triplet arch stem).
if defined _ARCH set "VS_ARCH=%_ARCH%"
call "%PROJECT_DIR%_build_env.cmd"
if errorlevel 1 exit /b 1
set "VCPKG_TRIPLET=%VCPKG_ARCH%-windows"

:: Resolve backend: CLI token, else the POSTGRESQL default. Then derive the
:: short dir tag (the default POSTGRESQL has none, so its dir is plain "build").
if not defined _DB_BACKEND set "_DB_BACKEND=POSTGRESQL"
set "_DB_TAG="
if /I "%_DB_BACKEND%"=="POSTGRESQL_LEGACY" set "_DB_TAG=legacy"
if /I "%_DB_BACKEND%"=="MYSQL"      set "_DB_TAG=mysql"
if /I "%_DB_BACKEND%"=="SQLITE"     set "_DB_TAG=sqlite"
if /I "%_DB_BACKEND%"=="DUCKDB"     set "_DB_TAG=duckdb"
if /I "%_DB_BACKEND%"=="CLICKHOUSE" set "_DB_TAG=ck"

:: Build dir: build[_<arch>][_<tag>]. The arch suffix is dropped when targeting
:: the host arch (the common case); the backend suffix is dropped for the
:: POSTGRESQL default. So the plain host+postgresql build is just "build".
set "_ARCH_SUFFIX="
if /I not "%VS_ARCH%"=="%HOST_ARCH%" set "_ARCH_SUFFIX=_%VS_ARCH%"
set "_TAG_SUFFIX="
if defined _DB_TAG set "_TAG_SUFFIX=_%_DB_TAG%"
set "BUILD_DIR=%PROJECT_DIR%build%_ARCH_SUFFIX%%_TAG_SUFFIX%"
set "_DB_BACKEND_ARG=-DMIROBODY_DATABASE_BACKEND=%_DB_BACKEND%"

:: clean: wipe the build dir so the next run reconfigures from scratch. vcpkg
:: dependencies live in the shared %PROJECT_DIR%vcpkg_installed (see
:: VCPKG_INSTALLED_DIR below), not inside the build dir, so this never triggers a
:: dependency rebuild -- it just drops stale CMake cache / object files.
if defined _CLEAN if exist "%BUILD_DIR%" rd /s /q "%BUILD_DIR%"

:: Optional shared vcpkg binary cache. Point MIROBODY_VCPKG_CACHE at a directory
:: (local, or a team share / NAS) to reuse prebuilt packages across machines and
:: build dirs -- vcpkg pulls binaries from there instead of compiling from source.
if defined MIROBODY_VCPKG_CACHE (
    if not exist "%MIROBODY_VCPKG_CACHE%" mkdir "%MIROBODY_VCPKG_CACHE%"
    set "VCPKG_DEFAULT_BINARY_CACHE=%MIROBODY_VCPKG_CACHE%"
)

if not exist "%BUILD_DIR%\build.ninja" (
    cmake -S "%PROJECT_DIR%." -B "%BUILD_DIR%" ^
        -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
        -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
        -DVCPKG_TARGET_TRIPLET=%VCPKG_TRIPLET% ^
        -DVCPKG_INSTALLED_DIR="%PROJECT_DIR%vcpkg_installed" ^
        %_DB_BACKEND_ARG%
    if errorlevel 1 exit /b 1
)

cmake --build "%BUILD_DIR%" --config Release
goto :eof

:usage
echo Usage: build.cmd [arch] [backend] [clean]      (tokens in any order)
echo.
echo   With no arguments: build the host arch with the POSTGRESQL default
echo   (into "build"). Pass help / -h / --help / /? to show this help.
echo.
echo   Arch      amd64 (or x86_64), arm64, x86       default: host arch
echo   Backend   pg / postgresql, legacy / pg_legacy, mysql, sqlite, ck / clickhouse, duckdb
echo             (omit for the POSTGRESQL default)
echo   clean     clear CMake's cache (keep the dir's vcpkg_installed) and
echo             reconfigure -- use after changing options or moving the repo
echo   help / -h / --help / /?   show this help
echo.
echo The build dir is build[_^<arch^>][_^<backend^>]: the arch suffix is omitted for
echo the host arch, the backend suffix for the POSTGRESQL default. Examples
echo below assume an amd64 host, so each combo gets its own coexisting dir:
echo   build.cmd                 -^> build               (host arch, POSTGRESQL)
echo   build.cmd legacy          -^> build_legacy        (host arch, POSTGRESQL_LEGACY)
echo   build.cmd sqlite          -^> build_sqlite        (host arch, SQLITE)
echo   build.cmd arm64           -^> build_arm64         (cross-arch, POSTGRESQL)
echo   build.cmd arm64 legacy    -^> build_arm64_legacy  (cross-arch, POSTGRESQL_LEGACY)
echo.
echo Environment variables (see README "Building - Windows"):
echo   VS_DIR       Visual Studio install root.
echo                Default: C:\Program Files\Microsoft Visual Studio\18\Community
echo   VCPKG_ROOT   vcpkg checkout. If unset, vcvarsall.bat points at VS-bundled vcpkg.
echo   NINJA        ninja.exe path.
echo                Default: %%VS_DIR%%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
echo   MIROBODY_VCPKG_CACHE  Optional dir for a shared vcpkg binary cache, reused
echo                across machines and build dirs. Created if missing.
echo.
echo Arch and backend come only from the command line (no env vars).
exit /b 0
