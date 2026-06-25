@echo off
setlocal EnableExtensions

if /I "%1"=="/?"      goto :usage
if /I "%1"=="-h"      goto :usage
if /I "%1"=="--help"  goto :usage
if /I "%1"=="help"    goto :usage

:: Build the Python wheel (mirobody._mirobody) via scikit-build-core + pybind11.
:: See python/README.md.

set "PROJECT_DIR=%~dp0"

:: Shared toolchain bring-up: VS_DIR/NINJA defaults, VS_ARCH, VCPKG_ARCH (triplet
:: arch stem), vcvars, VCPKG_ROOT. Same env vars as build.cmd.
call "%PROJECT_DIR%_build_env.cmd"
if errorlevel 1 exit /b 1

:: Static native deps but dynamic MSVC runtime (/MD): the *-static-md triplet links
:: curl / OpenSSL / libwebsockets / ... into _mirobody.pyd while keeping the /MD CRT
:: that CPython extensions require. The default dynamic *-windows triplet would
:: leave the .pyd depending on loose vcpkg DLLs not shipped in the wheel, so
:: `import mirobody` would fail on machines without them on PATH.
if not defined VCPKG_TRIPLET set "VCPKG_TRIPLET=%VCPKG_ARCH%-windows-static-md"

:: Put the VS-bundled Ninja on PATH and force the single-config Ninja generator.
:: Left to its own heuristics, scikit-build-core picked "NMake Makefiles" here and
:: built Debug (debug CRT + -d vcpkg libs), producing an unloadable module.
:: CMAKE_GENERATOR is honored by both scikit-build-core and CMake itself.
for %%I in ("%NINJA%") do set "NINJA_DIR=%%~dpI"
set "PATH=%NINJA_DIR%;%PATH%"
set "CMAKE_GENERATOR=Ninja"

:: Forward the vcpkg toolchain + triplet AND a hard Release build type to CMake.
:: SKBUILD_CMAKE_ARGS is semicolon-separated, so spaces in "Program Files" survive
:: intact, and these args are appended last so they win.
set "SKBUILD_CMAKE_ARGS=-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake;-DVCPKG_TARGET_TRIPLET=%VCPKG_TRIPLET%;-DCMAKE_BUILD_TYPE=Release"

:: Reusable build tree (build-* is gitignored), so reconfigures are incremental.
set "SKBUILD_BUILD_DIR=%PROJECT_DIR%build-python"

:: clean: drop CMake's configuration but KEEP build-python\vcpkg_installed, so the
:: wheel build below reconfigures without the slow vcpkg dependency rebuild. Also
:: fixes stale absolute paths in CMakeCache.txt after a repo move.
if /I "%1"=="clean" (
    if exist "%SKBUILD_BUILD_DIR%" (
        for /d %%D in ("%SKBUILD_BUILD_DIR%\*") do if /I not "%%~nxD"=="vcpkg_installed" rd /s /q "%%D"
        del /q "%SKBUILD_BUILD_DIR%\*" 2>nul
    )
)

:: Optional shared vcpkg binary cache (see build.cmd / README).
if defined MIROBODY_VCPKG_CACHE (
    if not exist "%MIROBODY_VCPKG_CACHE%" mkdir "%MIROBODY_VCPKG_CACHE%"
    set "VCPKG_DEFAULT_BINARY_CACHE=%MIROBODY_VCPKG_CACHE%"
)

python -m pip wheel "%PROJECT_DIR%." --wheel-dir "%PROJECT_DIR%dist"
exit /b %errorlevel%

:usage
echo Usage: build-python.cmd [clean^|help]
echo.
echo   clean       reconfigure from scratch (keeps build-python\vcpkg_installed)
echo   help / -h / --help / /?   show this help
echo.
echo   Builds the mirobody Python wheel into dist\ using the VS-bundled vcpkg
echo   toolchain (same setup as build.cmd). Shares the toolchain bring-up and env
echo   vars with build.cmd (VS_DIR / VCPKG_ROOT / NINJA / MIROBODY_VCPKG_CACHE).
exit /b 0
