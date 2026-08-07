@echo off
rem ---------------------------------------------------------------------------
rem Cross-compile llama.cpp for HarmonyOS and assemble an SDK dir the app links.
rem
rem   build-llama.cmd [abi] [arch] [clean]                (tokens in any order)
rem
rem     abi      arm64-v8a (default) | x86_64
rem     arch     the -march feature set. Default is read off a REAL DEVICE, not
rem              guessed: Kirin 9020 reports fp16+dotprod+i8mm+bf16+sve via AT_HWCAP
rem              (see nativeLocalStatus). Pass `baseline` for plain armv8-a, or any
rem              GGML_CPU_ARM_ARCH string.
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
rem Output: harmony\prebuilt\llama-sdk\<abi>\{include,lib}, which the app's CMakeLists
rem finds BY ITSELF -- the same contract as the vcpkg deps in harmony\prebuilt\<abi>,
rem and deliberately under the same parent: `prebuilt\` should mean every prebuilt
rem artifact, not just the ones vcpkg made, or a reader who sees it will assume the
rem engine is in there too. Build it and the on-device lane is on; delete it and the
rem core compiles its stub and the app still builds. Nothing to paste into
rem entry\build-profile.json5. Static archives: a HAP has nowhere to put a companion
rem .so, so the engine links into libmirobody.so.
rem
rem CPU ONLY, deliberately -- there is no backend option. llama.cpp's vulkan backend
rem built and genuinely ran here (it registered an igpu on a Kirin 9020: devices
rem "Vulkan0(igpu), CPU(cpu)", so this was not a silent fallback) and it lost on
rem every axis measured:
rem
rem   Qwen3.5-4B-Q4_K_M      CPU      Vulkan
rem   load                   ~2 s     11.7 s
rem   prefill                ~20/s    0.83/s   (19 tok in 22.9 s)
rem   decode                 ~5.2/s   1.47/s
rem
rem and switching models mid-session hung past two minutes with no token. The cause
rem is structural, not a tuning miss: n_gpu_layers offload copies every tensor into
rem device memory, where the CPU path just mmaps the GGUF and pages it in lazily, and
rem Maleoon shares its bandwidth with the CPU it is supposedly relieving -- while the
rem CPU side is running hand-written i8mm kernels. It also cost ~50 MB of SPIR-V in
rem the HAP: 3185 shader variants covering every quant x every op, gated on what the
rem HOST glslc can compile rather than what the target GPU can run, and not tunable
rem from here. So the option and its glslc / VULKAN_SDK / SPIRV-Headers plumbing are
rem gone rather than kept switched off; `git log -p -- harmony/build-llama.cmd` has
rem all of it if a different SoC or driver ever makes it worth re-measuring against
rem the numbers above. (The desktop Qt build still offers vulkan and cuda, where the
rem GPU has its own memory and wins -- see qt\build-qt.cmd.)
rem
rem Env overrides (the same set build-llama.sh takes):
rem   LLAMA_SRC      llama.cpp checkout (default: llama.cpp beside the repo)
rem   LLAMA_SDK_DIR  where to assemble the SDK (default: harmony\prebuilt\llama-sdk\<abi>).
rem                  Moving it out of the tree means naming it with -DLLAMA_CPP_DIR
rem                  in entry\build-profile.json5 -- the auto-detect only looks in
rem                  the default place.
rem   OHOS_SDK_ROOT  the SDK dir CONTAINING native\ (same meaning as in build-prebuilt.cmd);
rem                  wins over DEVECO_HOME
rem   DEVECO_HOME    DevEco Studio install (default: %ProgramFiles%\Huawei\DevEco Studio)
rem ---------------------------------------------------------------------------
setlocal EnableExtensions EnableDelayedExpansion

rem Tokens are position-independent apart from abi-before-arch, because
rem `build-llama.cmd arm64-v8a clean` is what one naturally types and treating
rem `clean` as the arch silently produced `-march=clean`. `cpu` is accepted and
rem ignored: it used to be the backend token, so it is all over older notes, and as
rem an arch it would mean `-march=cpu`.
set "ABI="
set "ARCH="
set "CLEAN="
set "ARCH_SET="
:parse
if "%~1"=="" goto parsed
if /I "%~1"=="clean" (
    set "CLEAN=clean"
) else if /I "%~1"=="cpu" (
    rem accepted for compatibility, and the only backend there is
) else if /I "%~1"=="vulkan" (
    echo [build-llama] there is no vulkan backend here any more -- CPU only.
    echo [build-llama] the measurements that decided it are in this file's header.
    exit /b 2
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

rem Unknown ABIs are rejected here instead of reaching -DOHOS_ARCH, where they fail
rem deep in the toolchain file with nothing pointing back at the argument.
set "ABI_OK="
if /I "%ABI%"=="arm64-v8a" set "ABI_OK=1"
if /I "%ABI%"=="x86_64"    set "ABI_OK=1"
if not defined ABI_OK (
    echo [build-llama] abi must be arm64-v8a^|x86_64 ^(got '%ABI%'^)
    exit /b 2
)

rem Default is the best measured config on a Kirin 9020: vs baseline armv8-a it is
rem ~2.95x prefill and ~1.42x decode. `+sve` is deliberately absent -- the device
rem has SVE1, and adding it REGRESSED prefill by ~34% (measured), apparently by
rem displacing the i8mm SMMLA kernels with SVE ones that have no width advantage
rem at 128-bit vectors. Pass an explicit arch to try something else.
if not defined ARCH_SET set "ARCH=armv8.6-a+i8mm+bf16+dotprod+fp16"

rem --- llama.cpp source: cloned once BY HAND, reused -------------------------
rem Never cloned for you here (qt\build-qt.cmd does clone; this one only prints the
rem command): the on-device numbers above were measured against a specific checkout,
rem so it wants one you pinned, not one silently refreshed under it.
rem One checkout for the whole repo, beside it rather than inside: qt\build-qt.cmd and
rem fine-tuning\train_units.py resolve LLAMA_SRC the same way, so a clone made for any
rem one of them serves the others. They used to default to three different directories
rem -- a Downloads checkout here, an opt\ one for Qt -- and two of the three did not
rem exist, so each script sent you off to clone its own copy.
set "LLAMA_SRC=%LLAMA_SRC%"
if "%LLAMA_SRC%"=="" for %%I in ("%~dp0..\..") do set "LLAMA_SRC=%%~fI\llama.cpp"
if not exist "%LLAMA_SRC%\include\llama.h" (
    echo [build-llama] no llama.cpp at "%LLAMA_SRC%".
    echo [build-llama] clone it, or set LLAMA_SRC to an existing checkout:
    echo [build-llama]   git clone --depth 1 https://github.com/ggml-org/llama.cpp "%LLAMA_SRC%"
    exit /b 1
)

rem --- OpenHarmony native SDK: toolchain file + its own cmake/ninja ----------
rem OHOS_SDK_ROOT names the directory CONTAINING native\, the same meaning it has in
rem build-prebuilt.cmd and build-llama.sh. It used to be ignored HERE, which is the
rem worst shape: setting it made build-prebuilt use your SDK while this script quietly
rem went on using the DevEco default, so a single-SDK machine worked and a two-SDK
rem machine cross-linked without saying so. DEVECO_HOME is now just the first
rem candidate rather than a separate mechanism, followed by the install locations
rem build-prebuilt.cmd probes.
if not defined OHOS_SDK_ROOT (
    for %%D in (
        "%DEVECO_HOME%\sdk\default\openharmony"
        "%ProgramFiles%\Huawei\DevEco Studio\sdk\default\openharmony"
        "%LOCALAPPDATA%\OpenHarmony\Sdk\default\openharmony"
    ) do (
        if not defined OHOS_SDK_ROOT if exist "%%~D\native\build\cmake\ohos.toolchain.cmake" set "OHOS_SDK_ROOT=%%~D"
    )
)
set "NATIVE=%OHOS_SDK_ROOT%\native"
set "TOOLCHAIN=%NATIVE%\build\cmake\ohos.toolchain.cmake"
if not exist "%TOOLCHAIN%" (
    echo [build-llama] OHOS toolchain not found at "%TOOLCHAIN%".
    echo [build-llama] set OHOS_SDK_ROOT ^(the dir CONTAINING native\^), or DEVECO_HOME
    echo [build-llama] to your DevEco Studio install.
    exit /b 1
)

rem The SDK ships its own cmake/ninja beside the toolchain; fall back to the host's,
rem as build-llama.sh does. Accepting OHOS_SDK_ROOT means accepting a command-line-tools
rem SDK, and those do not always carry build-tools\cmake -- without this the missing
rem exe surfaced as cmd's "The system cannot find the path specified".
set "CMAKE=%NATIVE%\build-tools\cmake\bin\cmake.exe"
set "NINJA=%NATIVE%\build-tools\cmake\bin\ninja.exe"
if not exist "%CMAKE%" for %%I in (cmake.exe) do set "CMAKE=%%~$PATH:I"
if not exist "%NINJA%" for %%I in (ninja.exe) do set "NINJA=%%~$PATH:I"
if not exist "%CMAKE%" (
    echo [build-llama] need cmake ^(the SDK's build-tools\cmake\bin, or on PATH^).
    exit /b 1
)
if not exist "%NINJA%" (
    echo [build-llama] need ninja ^(the SDK's build-tools\cmake\bin, or on PATH^).
    exit /b 1
)

set "BUILD=%~dp0.llama-build\%ABI%"
rem In-tree under harmony\prebuilt\, for the same reason the vcpkg deps are: the app's
rem CMakeLists resolves it by repo-relative path, so there is no machine-local absolute
rem path to paste into a tracked file and nothing to hide behind skip-worktree. One
rem /prebuilt rule in .gitignore covers both, and `git clean -xdf` takes both -- each
rem re-derivable by re-running its own script. build-prebuilt only ever replaces its
rem own prebuilt\<abi>, so the two never step on each other.
if defined LLAMA_SDK_DIR (
    set "SDK=%LLAMA_SDK_DIR%"
) else (
    set "SDK=%~dp0prebuilt\llama-sdk\%ABI%"
)

if /I "%CLEAN%"=="clean" (
    echo [build-llama] cleaning
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
    if exist "%SDK%"   rmdir /s /q "%SDK%"
)

echo [build-llama] abi=%ABI%
if "%ARCH%"=="" (echo [build-llama] arch=baseline armv8-a) else (echo [build-llama] arch=%ARCH%)

rem GGML_NATIVE=OFF because probing the host says nothing about the target.
rem LLAMA_CURL=OFF: the app downloads models itself; curl here would drag in a
rem second, differently-configured copy of a dependency the core already links.
rem Static libs only -- see the header comment.
set "ARCH_ARG="
if not "%ARCH%"=="" set "ARCH_ARG=-DGGML_CPU_ARM_ARCH=%ARCH%"

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
  %ARCH_ARG%
if errorlevel 1 (echo [build-llama] configure FAILED & exit /b 1)

rem Only the libraries: upstream's single `llama-app` CLI target links impl libs
rem that the OFF switches above remove, so asking for it would fail the link -- and
rem we want the library, not the CLI.
"%CMAKE%" --build "%BUILD%" --target llama ggml ggml-cpu ggml-base
if errorlevel 1 (echo [build-llama] build FAILED & exit /b 1)

echo [build-llama] assembling %SDK%
if not exist "%SDK%\include" mkdir "%SDK%\include"
if not exist "%SDK%\lib"     mkdir "%SDK%\lib"
copy /y "%LLAMA_SRC%\include\llama.h"      "%SDK%\include\" >nul
copy /y "%LLAMA_SRC%\ggml\include\*.h"     "%SDK%\include\" >nul
for %%L in (libllama.a) do copy /y "%BUILD%\src\%%L" "%SDK%\lib\" >nul
for %%L in (libggml.a libggml-cpu.a libggml-base.a) do copy /y "%BUILD%\ggml\src\%%L" "%SDK%\lib\" >nul

echo [build-llama] done: %SDK%
if defined LLAMA_SDK_DIR (
    echo [build-llama] that is NOT the auto-detected location, so name it in
    echo [build-llama] entry\build-profile.json5:
    echo [build-llama]   "arguments": "-DLLAMA_CPP_DIR=%SDK%"
) else (
    echo [build-llama] the app picks this up by itself -- rebuild the HAP to link it in.
)
endlocal
