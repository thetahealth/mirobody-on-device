@echo off
:: Shared environment bring-up for build*.cmd. Invoked via `call`, so it has no
:: setlocal and its variable assignments propagate back to the caller.
::
:: Sets VS_DIR / NINJA defaults; resolves VS_ARCH (the caller may preset it from a
:: CLI token, else the host arch); exposes HOST_ARCH and VCPKG_ARCH (the vcpkg
:: triplet arch stem, amd64 -> x64); runs vcvarsall; and preserves the user's
:: VCPKG_ROOT (vcvars otherwise overwrites it with the VS-bundled vcpkg).
::
:: The caller composes its own VCPKG_TRIPLET from %VCPKG_ARCH% -- e.g.
:: %VCPKG_ARCH%-windows (build), -windows-static (build-shared),
:: -windows-static-md (build-python).

if not defined VS_DIR set "VS_DIR=%ProgramFiles%\Microsoft Visual Studio\18\Community"
if not defined NINJA set "NINJA=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

:: Host arch, normalized to amd64 | arm64 | x86.
if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
    set "HOST_ARCH=arm64"
) else if /I "%PROCESSOR_ARCHITECTURE%"=="x86" (
    set "HOST_ARCH=x86"
) else (
    set "HOST_ARCH=amd64"
)

:: Target arch: a caller-preset VS_ARCH wins, else build for the host.
if not defined VS_ARCH set "VS_ARCH=%HOST_ARCH%"

:: vcpkg triplet arch stem: amd64 -> x64 (vcpkg has no amd64-windows); else as-is.
if /I "%VS_ARCH%"=="amd64" ( set "VCPKG_ARCH=x64" ) else ( set "VCPKG_ARCH=%VS_ARCH%" )

:: vcvarsall.bat overwrites VCPKG_ROOT with the VS-bundled copy; stash + restore
:: the user's value when they set one.
set "_USER_VCPKG_ROOT=%VCPKG_ROOT%"
call "%VS_DIR%\VC\Auxiliary\Build\vcvarsall.bat" %VS_ARCH% >nul
if errorlevel 1 exit /b 1
if defined _USER_VCPKG_ROOT set "VCPKG_ROOT=%_USER_VCPKG_ROOT%"
set "_USER_VCPKG_ROOT="
exit /b 0
