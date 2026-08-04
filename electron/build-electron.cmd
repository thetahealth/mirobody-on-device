@echo off
setlocal EnableExtensions
:: build-electron.cmd -- build + run/package the Mirobody Electron desktop app.
::
:: Electron-focused: it builds the web bundle (res/htdoc), installs the Electron deps
:: (electron, electron-builder, koffi, node-llama-cpp), then runs or packages. It does
:: NOT build the C++ shared library -- that needs the full vcpkg/MSVC toolchain; run
:: build-shared.cmd first (once) if build-shared\mirobody.dll is missing.
::
:: Usage: build-electron.cmd [run|dist] [clean]
::   run    (default)  -> npm start   (dev: embed the server, open the window)
::   dist              -> npm run dist (electron-builder package)
::   clean             -> wipe electron\node_modules before installing
::   -h / --help / /?  -> show this help

set "ELECTRON_DIR=%~dp0"
for %%I in ("%~dp0..") do set "ROOT=%%~fI"

set "_MODE=run"
set "_CLEAN="
:parse
if "%~1"=="" goto args_done
if /I "%~1"=="run"    ( set "_MODE=run"  & shift & goto parse )
if /I "%~1"=="dist"   ( set "_MODE=dist" & shift & goto parse )
if /I "%~1"=="clean"  ( set "_CLEAN=1"   & shift & goto parse )
if /I "%~1"=="-h"     goto usage
if /I "%~1"=="--help" goto usage
if /I "%~1"=="/?"     goto usage
echo [build-electron] Unknown token: %~1
goto usage
:args_done

:: --- 1. C++ shared library (embedded server, loaded via koffi) -- must be prebuilt ---
if not exist "%ROOT%\build-shared\mirobody.dll" (
    echo [build-electron] Missing "%ROOT%\build-shared\mirobody.dll".
    echo                   Build it once first ^(needs vcpkg/MSVC^):  build-shared.cmd
    exit /b 1
)

:: --- 2. Web bundle (htdoc -> res/htdoc), the renderer Electron loads ---
echo [build-electron] Building web bundle (htdoc)...
pushd "%ROOT%\htdoc"
call npm install
if errorlevel 1 ( popd & echo [build-electron] htdoc npm install failed. & exit /b 1 )
call npm run build
if errorlevel 1 ( popd & echo [build-electron] htdoc build failed. & exit /b 1 )
popd

:: --- 3. Electron deps ---
if defined _CLEAN if exist "%ELECTRON_DIR%node_modules" rd /s /q "%ELECTRON_DIR%node_modules"
pushd "%ELECTRON_DIR%"
if not exist node_modules (
    echo [build-electron] Installing Electron deps...
    call npm install
    if errorlevel 1 ( popd & echo [build-electron] npm install failed. & exit /b 1 )
)

:: --- 4. Run / package ---
if /I "%_MODE%"=="dist" (
    echo [build-electron] Packaging ^(electron-builder^)...
    call npm run dist
) else (
    echo [build-electron] Launching ^(npm start^)...
    call npm start
)
set "_RC=%ERRORLEVEL%"
popd
exit /b %_RC%

:usage
echo Usage: build-electron.cmd [run^|dist] [clean]
echo.
echo   Builds the web bundle + installs Electron deps, then runs or packages.
echo   Requires the C++ shared lib prebuilt (build-shared.cmd) -- not built here.
echo.
echo   run    (default)  npm start (dev)
echo   dist              npm run dist (electron-builder package)
echo   clean             wipe node_modules before installing
exit /b 0
