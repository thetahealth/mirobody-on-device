@echo off
setlocal EnableExtensions EnableDelayedExpansion
:: build-qt.cmd -- build the standalone Qt Quick desktop client (qt/) with MSVC.
::
:: qt/CMakeLists.txt has no project() call (it is designed to be add_subdirectory'd
:: from the top-level build, and links NOTHING from mirobody_core), so this script
:: generates a tiny wrapper CMakeLists and points CMake at that. The runnable exe
:: is build-qt\app\mirobody_qt.exe -- it MUST stay next to the generated Mirobody\
:: QML module directory (the "Mirobody" module is loaded from disk beside the exe,
:: NOT embedded), so we do not relocate it with CMAKE_RUNTIME_OUTPUT_DIRECTORY.
::
:: Usage: build-qt.cmd [clean] [deploy]   (tokens in any order)
::   clean    wipe the CMake cache and reconfigure from scratch
::   deploy   run windeployqt so the exe can launch by double-click
::   -h / --help / /?   show this help
::
:: Env overrides:
::   QT_PREFIX   Qt msvc kit dir     (default: D:\Qt\6.11.1\msvc2022_64)
::   VS_DIR      Visual Studio root  (default: C:\Program Files\Microsoft Visual Studio\18\Community)
::   NINJA       ninja.exe path      (default: D:\Qt\Tools\Ninja\ninja.exe)

set "PROJECT_DIR=%~dp0"
set "QT_SRC=%PROJECT_DIR%qt"
set "BUILD_DIR=%PROJECT_DIR%build-qt"
set "WRAP_DIR=%PROJECT_DIR%build-qt-wrap"
set "CACHE_DIR=%BUILD_DIR%"
set "EXE=%BUILD_DIR%\app\mirobody_qt.exe"

:: --- parse tokens ---
set "_CLEAN="
set "_DEPLOY="
:parse
if "%~1"=="" goto args_done
if /I "%~1"=="clean"   ( set "_CLEAN=1"  & shift & goto parse )
if /I "%~1"=="deploy"  ( set "_DEPLOY=1" & shift & goto parse )
if /I "%~1"=="-h"      goto usage
if /I "%~1"=="--help"  goto usage
if /I "%~1"=="/?"      goto usage
echo [build-qt] Unknown token: %~1
goto usage
:args_done

:: --- Qt kit ---
if not defined QT_PREFIX set "QT_PREFIX=D:\Qt\6.11.1\msvc2022_64"
if not exist "%QT_PREFIX%\lib\cmake\Qt6\Qt6Config.cmake" (
    echo [build-qt] Qt kit not found at "%QT_PREFIX%".
    echo            Set QT_PREFIX to your Qt msvc kit, e.g.:
    echo            set "QT_PREFIX=D:\Qt\6.11.1\msvc2022_64"
    exit /b 1
)

:: --- MSVC toolchain (vcvars puts cl.exe on PATH) ---
if not defined VS_DIR set "VS_DIR=C:\Program Files\Microsoft Visual Studio\18\Community"
set "VCVARS=%VS_DIR%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    echo [build-qt] vcvarsall.bat not found at "%VCVARS%".
    echo            Set VS_DIR to your Visual Studio install root.
    exit /b 1
)
if not defined NINJA set "NINJA=D:\Qt\Tools\Ninja\ninja.exe"

call "%VCVARS%" amd64 >nul
if errorlevel 1 ( echo [build-qt] vcvars failed. & exit /b 1 )

:: --- generate the wrapper CMakeLists (add_subdirectory the qt/ subtree) ---
if defined _CLEAN if exist "%CACHE_DIR%" rd /s /q "%CACHE_DIR%"
if not exist "%WRAP_DIR%" mkdir "%WRAP_DIR%"
set "QT_SRC_CMAKE=%QT_SRC:\=/%"
> "%WRAP_DIR%\CMakeLists.txt" echo cmake_minimum_required(VERSION 3.19)
>>"%WRAP_DIR%\CMakeLists.txt" echo project(mirobody_qt_standalone LANGUAGES C CXX)
>>"%WRAP_DIR%\CMakeLists.txt" echo add_subdirectory("%QT_SRC_CMAKE%" app)

:: --- configure + build. The exe lands in build-qt\app\ NEXT TO its generated
::     Mirobody\ QML module dir (loaded from disk), so we do NOT relocate it. ---
if not exist "%CACHE_DIR%\build.ninja" (
    cmake -S "%WRAP_DIR%" -B "%CACHE_DIR%" ^
        -G Ninja ^
        -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_PREFIX_PATH="%QT_PREFIX%"
    if errorlevel 1 exit /b 1
)

cmake --build "%CACHE_DIR%" --target mirobody_qt
if errorlevel 1 exit /b 1

echo [build-qt] Built: %EXE%

:: --- optional: bundle Qt runtime DLLs + QML plugins next to the exe ---
if defined _DEPLOY (
    echo [build-qt] Running windeployqt...
    "%QT_PREFIX%\bin\windeployqt.exe" --qmldir "%QT_SRC%\qml" "%EXE%"
    if errorlevel 1 exit /b 1
    echo [build-qt] Deployed. "%EXE%" is now runnable standalone.
)
goto :eof

:usage
echo Usage: build-qt.cmd [clean] [deploy]
echo.
echo   Builds qt/ (the standalone Qt Quick desktop client) with MSVC into build-qt\.
echo   Output: build-qt\app\mirobody_qt.exe
echo.
echo   clean    wipe the CMake cache and reconfigure from scratch
echo   deploy   run windeployqt so the exe launches by double-click
echo.
echo Env overrides: QT_PREFIX, VS_DIR, NINJA (see comments at top of this file).
exit /b 0
