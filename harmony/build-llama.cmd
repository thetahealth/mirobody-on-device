@echo off
rem ---------------------------------------------------------------------------
rem Cross-compile llama.cpp for HarmonyOS and assemble an SDK dir the app links.
rem
rem   build-llama.cmd [abi] [backend] [arch] [clean]      (tokens in any order)
rem
rem     abi      arm64-v8a (default) | x86_64
rem     backend  cpu (default) | vulkan. Each gets its OWN cached SDK, so switching
rem              cannot silently keep linking the previous one.
rem     arch     the -march feature set for the CPU backend. Default is read off a
rem              REAL DEVICE, not guessed: Kirin 9020 reports fp16+dotprod+i8mm+bf16
rem              +sve via AT_HWCAP (see nativeLocalStatus). Pass `baseline` for plain
rem              armv8-a, or any GGML_CPU_ARM_ARCH string.
rem     clean    discard the cached build tree and SDK first
rem
rem WHY THIS EXISTS: a cross build with no -march flags silently lands on baseline
rem armv8-a. CMake's feature probes compile-and-run, which is impossible when
rem cross-compiling, so HAVE_MATMUL_INT8 / HAVE_FP16_VECTOR_ARITHMETIC / HAVE_SVE
rem all "fail" and ggml quietly drops the fast kernels. On a Kirin 9020 that left
rem dotprod, i8mm, fp16 and SVE unused -- the Q4 matmul paths llama.cpp has hand
rem written kernels for. GGML_CPU_ARM_ARCH is the documented way to state the
rem target's features instead of probing for them.
rem
rem Output: D:\opt\llama-sdk-ohos-<abi>-<backend>\{include,lib}, static archives (a HAP
rem has nowhere to put a companion .so, so the engine is linked into libmirobody.so).
rem Feed it to the app with -DLLAMA_CPP_DIR in entry/build-profile.json5.
rem
rem USE `cpu`. The vulkan backend builds and genuinely runs -- it registers an igpu on
rem a Kirin 9020 (devices "Vulkan0(igpu), CPU(cpu)"), so this is not a silent fallback
rem -- and it loses on every axis measured against the CPU build above:
rem
rem   Qwen3.5-4B-Q4_K_M      CPU      Vulkan
rem   load                   ~2 s     11.7 s
rem   prefill                ~20/s    0.83/s   (19 tok in 22.9 s)
rem   decode                 ~5.2/s   1.47/s
rem
rem and switching models mid-session hung past two minutes with no token. The cause is
rem structural, not a tuning miss: n_gpu_layers offload copies every tensor into device
rem memory, where the CPU path just mmaps the GGUF and pages it in lazily, and Maleoon
rem shares its bandwidth with the CPU it is supposedly relieving -- while the CPU side
rem is running hand-written i8mm kernels. It also costs ~50 MB of SPIR-V in the HAP.
rem Kept buildable because a different SoC or driver could flip this; re-measure with
rem the numbers above before believing it did.
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion

rem Tokens are position-independent apart from abi-before-arch, because
rem `build-llama.cmd arm64-v8a clean` is what one naturally types and treating
rem `clean` as the arch silently produced `-march=clean`.
set "ABI="
set "ARCH="
set "CLEAN="
set "ARCH_SET="
set "BACKEND=cpu"
:parse
if "%~1"=="" goto parsed
if /I "%~1"=="clean" (
    set "CLEAN=clean"
) else if /I "%~1"=="cpu" (
    set "BACKEND=cpu"
) else if /I "%~1"=="vulkan" (
    set "BACKEND=vulkan"
) else if /I "%~1"=="baseline" (
    set "ARCH="
    set "ARCH_SET=1"
) else if "%ABI%"=="" (
    set "ABI=%~1"
) else (
    set "ARCH=%~1"
    set "ARCH_SET=1"
)
shift
goto parse
:parsed

if "%ABI%"=="" set "ABI=arm64-v8a"

rem The sysroot's per-target lib dir, which the vulkan loader below is read from.
rem Derived rather than hardcoded: `x86_64 vulkan` used to be handed the aarch64
rem path, which does not exist in an x86_64 sysroot. Unknown ABIs are rejected here
rem instead of reaching -DOHOS_ARCH, where they fail deep in the toolchain file.
set "OHOS_TRIPLE="
if /I "%ABI%"=="arm64-v8a" set "OHOS_TRIPLE=aarch64-linux-ohos"
if /I "%ABI%"=="x86_64"    set "OHOS_TRIPLE=x86_64-linux-ohos"
if not defined OHOS_TRIPLE (
    echo [build-llama] abi must be arm64-v8a^|x86_64 ^(got '%ABI%'^)
    exit /b 2
)

rem Default is the best measured config on a Kirin 9020: vs baseline armv8-a it is
rem ~2.95x prefill and ~1.42x decode. `+sve` is deliberately absent -- the device
rem has SVE1, and adding it REGRESSED prefill by ~34% (measured), apparently by
rem displacing the i8mm SMMLA kernels with SVE ones that have no width advantage
rem at 128-bit vectors. Pass an explicit arch to try something else.
if not defined ARCH_SET set "ARCH=armv8.6-a+i8mm+bf16+dotprod+fp16"

rem --- llama.cpp source: cloned once, reused ---------------------------------
set "LLAMA_SRC=%LLAMA_SRC%"
if "%LLAMA_SRC%"=="" set "LLAMA_SRC=D:\Downloads\llama.cpp"
if not exist "%LLAMA_SRC%\include\llama.h" (
    echo [build-llama] no llama.cpp at "%LLAMA_SRC%".
    echo [build-llama] clone it, or set LLAMA_SRC to an existing checkout:
    echo [build-llama]   git clone --depth 1 https://github.com/ggml-org/llama.cpp "%LLAMA_SRC%"
    exit /b 1
)

rem --- DevEco native SDK: toolchain file + its own cmake/ninja ---------------
set "DES=%DEVECO_HOME%"
if "%DES%"=="" set "DES=D:\Huawei\DevEco Studio"
set "NATIVE=%DES%\sdk\default\openharmony\native"
set "TOOLCHAIN=%NATIVE%\build\cmake\ohos.toolchain.cmake"
set "CMAKE=%NATIVE%\build-tools\cmake\bin\cmake.exe"
set "NINJA=%NATIVE%\build-tools\cmake\bin\ninja.exe"
if not exist "%TOOLCHAIN%" (
    echo [build-llama] OHOS toolchain not found at "%TOOLCHAIN%".
    echo [build-llama] set DEVECO_HOME to your DevEco Studio install.
    exit /b 1
)

rem Ninja must be findable BY NAME, not just via CMAKE_MAKE_PROGRAM: the Vulkan
rem backend builds its shader generator as a HOST tool through ExternalProject, and
rem that nested configure inherits the generator but not the make program, so it
rem fails with "unable to find a build program corresponding to Ninja".
set "PATH=%NATIVE%\build-tools\cmake\bin;%PATH%"

set "BUILD=%~dp0.llama-build\%ABI%-%BACKEND%"
rem One SDK per backend: a switch must not silently link the previous build.
set "SDK=D:\opt\llama-sdk-ohos-%ABI%-%BACKEND%"

if /I "%CLEAN%"=="clean" (
    echo [build-llama] cleaning
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
    if exist "%SDK%"   rmdir /s /q "%SDK%"
)

echo [build-llama] abi=%ABI% backend=%BACKEND%
if "%ARCH%"=="" (echo [build-llama] arch=baseline armv8-a) else (echo [build-llama] arch=%ARCH%)

rem GGML_NATIVE=OFF because probing the host says nothing about the target.
rem LLAMA_CURL=OFF: the app downloads models itself; curl here would drag in a
rem second, differently-configured copy of a dependency the core already links.
rem Static libs only -- see the header comment.
set "ARCH_ARG="
if not "%ARCH%"=="" set "ARCH_ARG=-DGGML_CPU_ARM_ARCH=%ARCH%"

rem Vulkan: OHOS ships the loader and headers in its own sysroot, so the only piece
rem that must come from the host is glslc -- the shader compiler runs at BUILD time
rem and is not part of the cross toolchain. Point CMake at the sysroot's Vulkan
rem explicitly; left to itself it would find the HOST SDK's and link the wrong ABI.
set "VK_ARG="
if /I "%BACKEND%"=="vulkan" (
    rem A preset GLSLC wins -- the message below offers that override, and clearing
    rem the variable first made it a no-op. Otherwise: newest SDK under D:\VulkanSDK.
    if not defined GLSLC for /f "delims=" %%G in ('dir /b /o-n "D:\VulkanSDK" 2^>nul') do (
        if not defined GLSLC if exist "D:\VulkanSDK\%%G\Bin\glslc.exe" set "GLSLC=D:\VulkanSDK\%%G\Bin\glslc.exe"
    )
    if not defined GLSLC (
        echo [build-llama] vulkan needs glslc; install the Vulkan SDK or set GLSLC.
        exit /b 1
    )
    echo [build-llama] glslc=!GLSLC!
    rem NOTE: no parentheses in comments inside this if-block -- a `rem` containing
    rem `^)` closes the block early, which cmd then reports as a bogus command.
    rem ggml-vulkan also wants SPIRV-Headers, found via $ENV{VULKAN_SDK}. Those are
    rem HOST build-time headers for compiling shaders, not target libraries, but the
    rem OHOS toolchain sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE to ONLY and hides
    rem anything outside the sysroot -- hence the override below.
    for %%G in ("!GLSLC!\..\..") do set "VULKAN_SDK=%%~fG"
    echo [build-llama] VULKAN_SDK=!VULKAN_SDK!
    rem Every path is quoted: the SDK lives under "DevEco Studio", and unquoted the
    rem space truncated it to D:/Huawei/DevEco, which CMake then "found" as the Vulkan
    rem library before failing on the include dir.
    rem Headers come from the HOST SDK, the loader from the OHOS sysroot. ggml-vulkan
    rem includes vulkan.hpp -- the C++ bindings -- and the OHOS sysroot ships only the C
    rem headers, so the sysroot alone cannot build it. Mixing is sound here: vulkan.hpp
    rem is header-only and platform-independent, Vulkan's ABI is stable, and the loader
    rem negotiates its version at runtime, so newer headers against an older loader is
    rem fine as long as nothing calls a newer entry point -- and ggml-vulkan feature
    rem checks at runtime. Watch this if a device reports a much older Vulkan than the
    rem host SDK: the device here reports 1.4.309 against a 1.4.350 SDK.
    rem Size warning: this backend adds ~50 MB to the .so, almost all of it SPIR-V --
    rem 3185 shader variants covering every quant x every op, though the app runs one
    rem model in one quant. It is not tunable from here. The variants are gated on what
    rem the HOST glslc can compile rather than what the target GPU can run (the mirror
    rem image of the -march problem above: there the host probes failed and cost
    rem performance, here they succeed and embed dead code), and the probe overwrites
    rem its own result variable, so -DGGML_VULKAN_*_GLSLC_SUPPORT=OFF is silently a
    rem no-op. Measured payoff for the one provably-dead family, NVIDIA's
    rem cooperative_matrix2: 3.3 MB of 44. So the 50 MB is the price of the backend,
    rem and the only question that matters is whether it buys throughput on Maleoon --
    rem if not, drop `vulkan` and the whole cost goes with it.
    set "VK_ARG=-DGGML_VULKAN=ON -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH -DVulkan_GLSLC_EXECUTABLE="!GLSLC!" -DVulkan_INCLUDE_DIR="!VULKAN_SDK!/Include" -DVulkan_LIBRARY="!NATIVE!/sysroot/usr/lib/!OHOS_TRIPLE!/libvulkan.so" -DSPIRV-Headers_DIR="!VULKAN_SDK!/Lib/cmake/SPIRV-Headers""
)

"%CMAKE%" -S "%LLAMA_SRC%" -B "%BUILD%" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" ^
  -DOHOS_ARCH=%ABI% ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SHARED_LIBS=OFF ^
  -DGGML_NATIVE=OFF ^
  -DGGML_OPENMP=OFF ^
  -DLLAMA_CURL=OFF ^
  -DLLAMA_BUILD_TESTS=OFF ^
  -DLLAMA_BUILD_EXAMPLES=OFF ^
  -DLLAMA_BUILD_SERVER=OFF ^
  -DLLAMA_BUILD_TOOLS=OFF ^
  %ARCH_ARG% !VK_ARG!
if errorlevel 1 (echo [build-llama] configure FAILED & exit /b 1)

rem Only the libraries: upstream's single `llama-app` CLI target links impl libs
rem that the OFF switches above remove, so asking for it would fail the link -- and
rem we want the library, not the CLI.
set "TARGETS=llama ggml ggml-cpu ggml-base"
if /I "%BACKEND%"=="vulkan" set "TARGETS=!TARGETS! ggml-vulkan"
"%CMAKE%" --build "%BUILD%" --target !TARGETS!
if errorlevel 1 (echo [build-llama] build FAILED & exit /b 1)

echo [build-llama] assembling %SDK%
if not exist "%SDK%\include" mkdir "%SDK%\include"
if not exist "%SDK%\lib"     mkdir "%SDK%\lib"
copy /y "%LLAMA_SRC%\include\llama.h"      "%SDK%\include\" >nul
copy /y "%LLAMA_SRC%\ggml\include\*.h"     "%SDK%\include\" >nul
for %%L in (libllama.a) do copy /y "%BUILD%\src\%%L" "%SDK%\lib\" >nul
for %%L in (libggml.a libggml-cpu.a libggml-base.a) do copy /y "%BUILD%\ggml\src\%%L" "%SDK%\lib\" >nul
if /I "%BACKEND%"=="vulkan" (
    rem The backend lives in its own subdirectory and its own archive.
    for /r "%BUILD%\ggml\src" %%L in (libggml-vulkan.a) do copy /y "%%L" "%SDK%\lib\" >nul
)

echo [build-llama] done: %SDK%
echo [build-llama] the app picks it up via -DLLAMA_CPP_DIR in entry/build-profile.json5
endlocal
