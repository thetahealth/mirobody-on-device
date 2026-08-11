@echo off
setlocal EnableExtensions EnableDelayedExpansion
:: build-qt.cmd -- build the Qt Quick desktop client (qt/) with MSVC, optionally with
:: the on-device engine (llama.cpp). build-qt only *builds*: it never downloads a model.
:: Models are chosen at runtime in the app (gear -> On-device AI); the app ships with a
:: sensible default. So the only build choice is the compute backend, which build-qt
:: builds + caches the llama.cpp SDK for (per backend).
::
:: qt/CMakeLists.txt has no project() call, so we generate a tiny add_subdirectory
:: wrapper. The exe (build-qt\app\mirobody_qt.exe) stays next to its generated
:: Mirobody\ QML module dir (loaded from disk), so we do not relocate it.
::
:: Usage: build-qt.cmd [clean] [deploy] [<backend>]  (any order)
::   <backend>  cpu | avx2 | vulkan | cuda  -- giving one ENABLES the on-device engine:
::                cpu    portable CPU baseline (GGML_NATIVE=OFF)
::                avx2   CPU tuned to this machine (GGML_NATIVE=ON)
::                vulkan GPU via Vulkan (needs the Vulkan SDK; Intel Arc / AMD / NVIDIA)
::                cuda   NVIDIA GPU via CUDA (needs the CUDA Toolkit; fastest on NVIDIA)
::   clean      wipe caches and reconfigure/rebuild from scratch (also forces a
::              llama.cpp rebuild). Needed when turning the on-device engine OFF.
::   deploy     run windeployqt so the exe launches by double-click
::   check      detect this machine's GPU/SDKs and recommend a backend, then exit
::   -h / --help / /?   show this help
::
:: Examples:
::   build-qt.cmd                  stub build, no on-device engine
::   build-qt.cmd cpu deploy       on-device, portable CPU, deployed
::   build-qt.cmd vulkan deploy    on-device, GPU via Vulkan
::
:: Env overrides:
::   QT_ROOT     Qt install root      (default: %SystemDrive%\Qt; scanned for a kit)
::   QT_PREFIX   Qt msvc kit dir      (default: newest %QT_ROOT%\6.*\msvc*)
::   VS_DIR      Visual Studio root   (default: %ProgramFiles%\Microsoft Visual Studio\18\Community)
::   NINJA       ninja.exe path       (default: %QT_ROOT%\Tools\Ninja\ninja.exe)
::   LLAMA_SRC       llama.cpp checkout (default: llama.cpp beside the repo; cloned if missing)
::   LLAMA_CPP_DIR   use a prebuilt llama.cpp SDK (include/,lib/,bin/) instead of building one

:: This script lives in qt/. QT_SRC is its own dir; the build output goes to the
:: repo root (alongside build/, build-legacy/, ...), covered by .gitignore build-*/.
for %%I in ("%~dp0.")  do set "QT_SRC=%%~fI"
for %%I in ("%~dp0..") do set "PROJECT_DIR=%%~fI"
set "BUILD_DIR=%PROJECT_DIR%\build-qt"
set "WRAP_DIR=%PROJECT_DIR%\build-qt-wrap"
set "CACHE_DIR=%BUILD_DIR%"
set "EXE=%BUILD_DIR%\app\mirobody_qt.exe"

:: --- parse tokens ---
set "_CLEAN="
set "_DEPLOY="
set "_BACKEND="
set "_CHECK="
:parse
if "%~1"=="" goto args_done
if /I "%~1"=="check"   ( set "_CHECK=1"        & shift & goto parse )
if /I "%~1"=="clean"   ( set "_CLEAN=1"        & shift & goto parse )
if /I "%~1"=="deploy"  ( set "_DEPLOY=1"       & shift & goto parse )
if /I "%~1"=="cpu"     ( set "_BACKEND=cpu"    & shift & goto parse )
if /I "%~1"=="avx2"    ( set "_BACKEND=avx2"   & shift & goto parse )
if /I "%~1"=="vulkan"  ( set "_BACKEND=vulkan" & shift & goto parse )
if /I "%~1"=="cuda"    ( set "_BACKEND=cuda"   & shift & goto parse )
if /I "%~1"=="-h"      goto usage
if /I "%~1"=="--help"  goto usage
if /I "%~1"=="/?"      goto usage
echo [build-qt] Unknown token: %~1
goto usage
:args_done

:: --- `check`: report on-device backend options for this machine, then exit ---
if defined _CHECK ( call :detect_env & exit /b 0 )
if not defined _BACKEND echo [build-qt] Tip: run "build-qt.cmd check" to see on-device (GPU) options.

:: --- Qt kit ---
:: Nothing is baked in: the install root and the version installed are both per-machine.
:: QT_PREFIX names the kit outright; failing that, scan the newest 6.x under QT_ROOT for
:: an msvc kit, newest kit first. Same idiom as the DevEco/Vulkan scans in
:: harmony\build-llama.cmd -- discover, then say what was searched if it comes up empty.
if not defined QT_ROOT set "QT_ROOT=%SystemDrive%\Qt"
if not defined QT_PREFIX for /f "delims=" %%V in ('dir /b /ad /o-n "%QT_ROOT%\6.*" 2^>nul') do (
    if not defined QT_PREFIX for /f "delims=" %%K in ('dir /b /ad /o-n "%QT_ROOT%\%%V\msvc*" 2^>nul') do (
        if not defined QT_PREFIX if exist "%QT_ROOT%\%%V\%%K\lib\cmake\Qt6\Qt6Config.cmake" set "QT_PREFIX=%QT_ROOT%\%%V\%%K"
    )
)
if not exist "%QT_PREFIX%\lib\cmake\Qt6\Qt6Config.cmake" (
    echo [build-qt] Qt kit not found. Searched "%QT_ROOT%\6.*\msvc*".
    echo [build-qt] Set QT_PREFIX to your Qt msvc kit, or QT_ROOT to your Qt install root.
    exit /b 1
)
echo [build-qt] Qt kit: %QT_PREFIX%

:: --- MSVC toolchain (vcvars puts cl.exe on PATH; needed for llama.cpp too) ---
if not defined VS_DIR set "VS_DIR=%ProgramFiles%\Microsoft Visual Studio\18\Community"
set "VCVARS=%VS_DIR%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    echo [build-qt] vcvarsall.bat not found at "%VCVARS%". Set VS_DIR to your VS root.
    exit /b 1
)
:: Qt's own Ninja if the installer placed one; otherwise the copy vcvars puts on PATH.
if not defined NINJA if exist "%QT_ROOT%\Tools\Ninja\ninja.exe" set "NINJA=%QT_ROOT%\Tools\Ninja\ninja.exe"
if not defined NINJA set "NINJA=ninja.exe"
call "%VCVARS%" amd64 >nul
if errorlevel 1 ( echo [build-qt] vcvars failed. & exit /b 1 )

:: --- on-device: build/obtain the llama.cpp SDK for the chosen backend ---
:: (The default model is baked into the app; models are managed at runtime, not here.)
set "_ONDEV_ARGS="
if not defined _BACKEND goto skip_ondev
call :ensure_llama
if errorlevel 1 exit /b 1
:: Quoted, and plain `set` so the quotes survive into the value: the default SDK lives
:: under %USERPROFILE%, which contains a space on any machine whose account name does,
:: and cmake would take the tail as a separate argument -- failing much later, at the
:: find_library, with a path truncated at the space.
set _ONDEV_ARGS=-DMIROBODY_ONDEVICE_LLM=ON -DLLAMA_CPP_DIR="!LLAMA_CPP_DIR:\=/!"
echo [build-qt] on-device: backend=%_BACKEND%
:skip_ondev

:: --- generate the wrapper CMakeLists (add_subdirectory the qt/ subtree) ---
if defined _CLEAN if exist "%CACHE_DIR%" rd /s /q "%CACHE_DIR%"
if not exist "%WRAP_DIR%" mkdir "%WRAP_DIR%"
set "QT_SRC_CMAKE=%QT_SRC:\=/%"
> "%WRAP_DIR%\CMakeLists.txt" echo cmake_minimum_required(VERSION 3.19)
>>"%WRAP_DIR%\CMakeLists.txt" echo project(mirobody_qt_standalone LANGUAGES C CXX)
>>"%WRAP_DIR%\CMakeLists.txt" echo add_subdirectory("%QT_SRC_CMAKE%" app)

:: --- configure (always, so changed tokens take effect) + build ---
cmake -S "%WRAP_DIR%" -B "%CACHE_DIR%" ^
    -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%QT_PREFIX%" ^
    %_ONDEV_ARGS%
if errorlevel 1 exit /b 1

cmake --build "%CACHE_DIR%" --target mirobody_qt
if errorlevel 1 exit /b 1

:: On-device build needs the llama.cpp runtime DLLs next to the exe.
if defined _BACKEND copy /y "%LLAMA_CPP_DIR%\bin\*.dll" "%BUILD_DIR%\app\" >nul

echo [build-qt] Built: %EXE%

:: --- optional: bundle Qt runtime DLLs + QML plugins next to the exe ---
if defined _DEPLOY (
    echo [build-qt] Running windeployqt...
    "%QT_PREFIX%\bin\windeployqt.exe" --qmldir "%QT_SRC%\qml" "%EXE%"
    if errorlevel 1 exit /b 1
    echo [build-qt] Deployed. "%EXE%" is now runnable standalone.
)
goto :eof

:: ---------------------------------------------------------------------------
:: Build (or reuse) a llama.cpp SDK for %_BACKEND% and set LLAMA_CPP_DIR to it.
:: ---------------------------------------------------------------------------
:ensure_llama
if defined LLAMA_CPP_DIR ( echo [build-qt] using preset LLAMA_CPP_DIR=%LLAMA_CPP_DIR% & exit /b 0 )
rem Beside the repo, and resolved the same way harmony\build-llama.cmd and
rem fine-tuning\tool_train.py resolve it -- one checkout serves all three.
if not defined LLAMA_SRC for %%I in ("%PROJECT_DIR%\..") do set "LLAMA_SRC=%%~fI\llama.cpp"
:: Outside the repo -- it is a large build product, not a source artifact. LLAMA_CPP_DIR
:: (checked just above) points at an existing SDK instead.
set "LLAMA_CPP_DIR=%USERPROFILE%\opt\llama-sdk-%_BACKEND%"
rem Reuse is by PRESENCE, so a cached SDK outlives the checkout it came from -- and this
rem script never updates an existing llama.cpp clone either. src/llm/local.cpp needs the
rem API at upstream 935cad649 (2026-08-04) or newer; an older cache reaches the root
rem CMakeLists, which checks llama.h for it and says so. Pass `clean` to discard the cache.
if not defined _CLEAN if exist "%LLAMA_CPP_DIR%\lib\llama.lib" (
    echo [build-qt] reusing cached llama SDK: %LLAMA_CPP_DIR%
    exit /b 0
)
if /I "%_BACKEND%"=="vulkan" if not defined VULKAN_SDK (
    echo [build-qt] 'vulkan' backend needs the Vulkan SDK. Install it from
    echo            https://vulkan.lunarg.com/ ^(sets VULKAN_SDK^), then re-run.
    exit /b 1
)
if /I "%_BACKEND%"=="cuda" if not defined CUDA_PATH (
    echo [build-qt] 'cuda' backend needs the NVIDIA CUDA Toolkit ^(sets CUDA_PATH^).
    echo            Install from https://developer.nvidia.com/cuda-downloads, then re-run.
    exit /b 1
)
if not exist "%LLAMA_SRC%\CMakeLists.txt" (
    echo [build-qt] cloning llama.cpp -^> %LLAMA_SRC%
    git clone --depth 1 https://github.com/ggml-org/llama.cpp "%LLAMA_SRC%"
    if errorlevel 1 ( echo [build-qt] llama.cpp clone failed & exit /b 1 )
)
set "_LFLAGS=-DGGML_NATIVE=OFF"
if /I "%_BACKEND%"=="avx2"   set "_LFLAGS=-DGGML_NATIVE=ON"
if /I "%_BACKEND%"=="vulkan" set "_LFLAGS=-DGGML_VULKAN=ON"
if /I "%_BACKEND%"=="cuda"   set "_LFLAGS=-DGGML_CUDA=ON"
rem ggml-vulkan does find_package(SPIRV-Headers CONFIG REQUIRED) and puts %VULKAN_SDK% on
rem CMAKE_PREFIX_PATH to satisfy it -- which is not enough. Config mode looks under a
rem prefix for lib/cmake/<Name>*/, a directory NAMED AFTER THE PACKAGE; the LunarG SDK
rem drops every config flat into Lib\cmake\, so nothing matches and the configure dies on
rem a header-only package sitting right there. Name the directory outright when that is
rem the layout; a newer SDK that nests them properly is found without help.
rem (Plain `set`, not `set "..."`: the value has to carry its own quotes for a spaced SDK
rem path, and the quoted form would swallow them.)
if /I "%_BACKEND%"=="vulkan" if exist "%VULKAN_SDK%\Lib\cmake\SPIRV-HeadersConfig.cmake" (
    set _LFLAGS=!_LFLAGS! -DSPIRV-Headers_DIR="%VULKAN_SDK%\Lib\cmake"
)
set "_LBUILD=%LLAMA_SRC%\build-%_BACKEND%"
echo [build-qt] building llama.cpp (%_BACKEND%) -- first time takes a few minutes...
cmake -S "%LLAMA_SRC%" -B "%_LBUILD%" -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON ^
    -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=OFF ^
    %_LFLAGS%
if errorlevel 1 ( echo [build-qt] llama.cpp configure failed & exit /b 1 )
cmake --build "%_LBUILD%" --target llama --config Release
if errorlevel 1 ( echo [build-qt] llama.cpp build failed & exit /b 1 )
if not exist "%LLAMA_CPP_DIR%\include" mkdir "%LLAMA_CPP_DIR%\include"
if not exist "%LLAMA_CPP_DIR%\lib" mkdir "%LLAMA_CPP_DIR%\lib"
if not exist "%LLAMA_CPP_DIR%\bin" mkdir "%LLAMA_CPP_DIR%\bin"
copy /y "%LLAMA_SRC%\include\*.h"       "%LLAMA_CPP_DIR%\include\" >nul
copy /y "%LLAMA_SRC%\ggml\include\*.h"  "%LLAMA_CPP_DIR%\include\" >nul
copy /y "%_LBUILD%\src\llama.lib"       "%LLAMA_CPP_DIR%\lib\" >nul
copy /y "%_LBUILD%\ggml\src\*.lib"      "%LLAMA_CPP_DIR%\lib\" >nul
copy /y "%_LBUILD%\bin\*.dll"           "%LLAMA_CPP_DIR%\bin\" >nul
if /I "%_BACKEND%"=="cuda" (
    rem Bundle the CUDA runtime so the exe runs without the toolkit on PATH.
    copy /y "%CUDA_PATH%\bin\cudart64_*.dll"   "%LLAMA_CPP_DIR%\bin\" >nul 2>&1
    copy /y "%CUDA_PATH%\bin\cublas64_*.dll"   "%LLAMA_CPP_DIR%\bin\" >nul 2>&1
    copy /y "%CUDA_PATH%\bin\cublasLt64_*.dll" "%LLAMA_CPP_DIR%\bin\" >nul 2>&1
)
echo [build-qt] llama SDK ready: %LLAMA_CPP_DIR%
exit /b 0

:: ---------------------------------------------------------------------------
:: Report which on-device backends this machine can build/run, and recommend one.
:: ---------------------------------------------------------------------------
:detect_env
set "_GPUS="
for /f "usebackq delims=" %%G in (`powershell -NoProfile -Command "(Get-CimInstance Win32_VideoController).Name -join '; '" 2^>nul`) do set "_GPUS=%%G"
echo %_GPUS% | findstr /i "NVIDIA" >nul && (set "_HASNV=1") || (set "_HASNV=")

echo.
echo [build-qt] On-device (Gemma) backend options for this machine:
echo   GPU detected : %_GPUS%
echo.
echo   cpu    : available            portable CPU baseline
echo   avx2   : available            native CPU build (faster; builds for this machine)
if defined VULKAN_SDK (
    echo   vulkan : READY               Vulkan SDK: %VULKAN_SDK%
) else (
    echo   vulkan : needs Vulkan SDK    https://vulkan.lunarg.com/
)
if defined _HASNV (
    if defined CUDA_PATH (
        echo   cuda   : READY               NVIDIA GPU + CUDA Toolkit: %CUDA_PATH%
    ) else (
        echo   cuda   : install Toolkit     NVIDIA GPU found; get CUDA: https://developer.nvidia.com/cuda-downloads
    )
) else (
    echo   cuda   : n/a                 no NVIDIA GPU
)

set "_REC=avx2"
if defined VULKAN_SDK set "_REC=vulkan"
if defined _HASNV if defined CUDA_PATH set "_REC=cuda"
echo.
echo   Recommended  : %_REC%
echo   Run          : qt\build-qt.cmd %_REC% deploy
echo.
exit /b 0

:usage
echo Usage: build-qt.cmd [clean] [deploy] [check] [cpu^|avx2^|vulkan^|cuda]
echo.
echo   Builds qt/ with MSVC into build-qt\app\mirobody_qt.exe. Give a backend token
echo   to enable the on-device engine (llama.cpp); build-qt builds and caches the
echo   llama.cpp SDK per backend. Models are chosen at runtime in the app, not here.
echo.
echo   check                 detect this machine's GPU/SDKs and recommend a backend
echo   cpu/avx2/vulkan/cuda  on-device backend (vulkan needs the Vulkan SDK;
echo                         cuda = NVIDIA, needs the CUDA Toolkit, fastest on NVIDIA)
echo   clean            wipe caches / force rebuild (needed to turn on-device OFF)
echo   deploy           run windeployqt so the exe is double-clickable
echo.
echo Env overrides: QT_ROOT, QT_PREFIX, VS_DIR, NINJA, LLAMA_SRC, LLAMA_CPP_DIR.
exit /b 0
