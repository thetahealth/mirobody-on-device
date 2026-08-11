@echo off
rem Cross-compile llama.cpp for Android into android/prebuilt/llama-sdk/<abi>/, which the
rem app's native build picks up by itself (see app/build.gradle.kts). The Android sibling
rem of harmony/build-llama.cmd, and deliberately the same shape: same output layout, same
rem switches, same reasons.
rem
rem   build-llama.cmd [abi] [clean]              (tokens in any order)
rem
rem     abi      arm64-v8a (default) | x86_64
rem     clean    discard the cached build tree and SDK first
rem
rem WHY NOT ONE -march: a cross build cannot probe the target -- CMake's feature tests
rem compile AND RUN, which is impossible here -- so ggml falls back to whatever -march
rem says and silently drops its fast kernels. That flag is worth more than any other
rem choice on this page: adding FEAT_I8MM took Qwen3 4B decode from 5.3 to 12.4 tok/s on
rem an 8 Elite Gen 5, a 2.3x swing from one instruction set.
rem
rem But a single flag cannot serve one APK. An i8mm binary SIGILLs on a Snapdragon 865,
rem which minSdk 26 still allows, so picking the fast one abandons those devices and
rem picking the safe one abandons the speed.
rem
rem So: GGML_CPU_ALL_VARIANTS. ggml builds one libggml-cpu-*.so per feature level, each
rem exporting ggml_backend_score(); at startup the loader dlopens them all, asks each what
rem it scores on THIS cpu, and keeps the winner. Seven variants cost ~6 MB stripped, which
rem is the whole price of never choosing wrong.
rem
rem That requires GGML_BACKEND_DL, which requires BUILD_SHARED_LIBS -- so this SDK is
rem shared objects, not the static archives the Harmony build produces, and the APK must
rem ship them as jniLibs with extractNativeLibs=true (the loader scans a directory for
rem real files; a library still inside the zip is not one). See app/build.gradle.kts.
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "ABI="
set "CLEAN="
for %%A in (%*) do (
    if /I "%%A"=="clean" ( set "CLEAN=1" ) else ( set "ABI=%%A" )
)
if not defined ABI set "ABI=arm64-v8a"

rem --- llama.cpp checkout ------------------------------------------------------
if not defined LLAMA_SRC (
    pushd "%~dp0..\.."
    set "LLAMA_SRC=!CD!\llama.cpp"
    popd
)
if not exist "%LLAMA_SRC%\include\llama.h" (
    echo [build-llama] no llama.cpp at "%LLAMA_SRC%".>&2
    echo [build-llama] clone it, or set LLAMA_SRC to an existing checkout:>&2
    echo [build-llama]   git clone --depth 1 https://github.com/ggml-org/llama.cpp "%LLAMA_SRC%">&2
    exit /b 1
)

rem --- NDK: prefer the space-free mirror build-prebuilt.cmd makes ---------------
rem A spaced NDK path makes the toolchain invoke clang through an 8.3 short name --
rem clang++.exe becomes CLANG_~1.EXE, which drops the "++" and links in C-driver mode.
rem Same reason build-app.cmd wants the mirror.
set "PINNED_NDK=27.0.12077973"
if not defined VCPKG_ROOT (
    pushd "%~dp0..\.."
    set "VCPKG_ROOT=!CD!\vcpkg"
    popd
)
if not defined MIROBODY_NDK_PATH (
    if exist "!VCPKG_ROOT!\android-ndk\%PINNED_NDK%\build\cmake\android.toolchain.cmake" (
        set "MIROBODY_NDK_PATH=!VCPKG_ROOT!\android-ndk\%PINNED_NDK%"
    )
)
if not defined MIROBODY_NDK_PATH (
    if defined ANDROID_NDK_HOME set "MIROBODY_NDK_PATH=%ANDROID_NDK_HOME%"
)
set "TOOLCHAIN=%MIROBODY_NDK_PATH%\build\cmake\android.toolchain.cmake"
if not exist "%TOOLCHAIN%" (
    echo [build-llama] NDK toolchain not found at "%TOOLCHAIN%".>&2
    echo [build-llama] run build-prebuilt.cmd first ^(it mirrors the NDK to a space-free path^),>&2
    echo [build-llama] or set MIROBODY_NDK_PATH.>&2
    exit /b 1
)

rem --- cmake + ninja: the SDK ships both under cmake/<ver>/bin -----------------
rem Where the SDK is, asked in the order that is most likely to be right: ANDROID_HOME,
rem then Gradle's own record of it (local.properties `sdk.dir`, written by Android Studio
rem and therefore present on any machine that has opened this project), then the default
rem install location. An SDK sits off the system drive as often as on it, so no literal
rem drive letter belongs here -- one that names the author's disk is a hardcode that
rem works exactly once.
if not defined CMAKE_BIN (
    set "SDK_ROOT=%ANDROID_HOME%"
    if not defined SDK_ROOT (
        rem Values there are .properties-escaped -- ":" written "\:", "\" written "\\" --
        rem so undo both. Only on this branch: a UNC path from ANDROID_HOME starts with
        rem the same "\\", and unescaping one would break it.
        for /f "tokens=1,* delims==" %%K in ('findstr /b /c:"sdk.dir=" "%~dp0local.properties" 2^>nul') do (
            set "SDK_ROOT=%%L"
            set "SDK_ROOT=!SDK_ROOT:\:=:!"
            set "SDK_ROOT=!SDK_ROOT:\\=\!"
        )
    )
    if not defined SDK_ROOT set "SDK_ROOT=%LOCALAPPDATA%\Android\Sdk"
    for /d %%D in ("!SDK_ROOT!\cmake\*") do (
        if exist "%%D\bin\cmake.exe" set "CMAKE_BIN=%%D\bin\cmake.exe"
        if exist "%%D\bin\ninja.exe" set "NINJA_BIN=%%D\bin\ninja.exe"
    )
)
if not defined CMAKE_BIN set "CMAKE_BIN=cmake"
if not defined NINJA_BIN set "NINJA_BIN=ninja"

rem --- Keep MSVC / Windows-SDK headers out of the cross-compile ----------------
set "INCLUDE="
set "LIB="
set "CPLUS_INCLUDE_PATH="
set "C_INCLUDE_PATH="

set "BUILD=%~dp0.llama-build\%ABI%"
if not defined LLAMA_SDK_DIR set "LLAMA_SDK_DIR=%~dp0prebuilt\llama-sdk\%ABI%"

if defined CLEAN (
    echo [build-llama] cleaning
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
    if exist "%LLAMA_SDK_DIR%" rmdir /s /q "%LLAMA_SDK_DIR%"
)

echo [build-llama] abi=%ABI%
echo [build-llama] cpu variants: all (chosen at runtime by ggml_backend_score)
echo [build-llama] ndk=%MIROBODY_NDK_PATH%

rem GGML_NATIVE=OFF because probing the host says nothing about the target.
rem LLAMA_CURL=OFF: the app downloads models itself; curl here would drag in a second,
rem differently-configured copy of a dependency the core already links.
rem c++_shared to match app/build.gradle.kts, or two STLs meet at link time.
rem
rem max-page-size=16384 for the same reason libmirobody.so needs it: a .so whose LOAD
rem segments are 4 KB-aligned will not load at all on an Android 15+ device running a
rem 16 KB kernel page size. ggml's own build does not set it, and nothing warns.
rem
rem MODULE as well as SHARED, and that is not belt-and-braces: ggml builds the CPU
rem variants with add_library(... MODULE), which takes CMAKE_MODULE_LINKER_FLAGS and
rem ignores the SHARED one entirely. Setting only SHARED aligns libllama/libggml to
rem 16 KB and leaves all seven variants at 4 KB -- i.e. exactly the libraries this whole
rem build exists to ship, silently unloadable. Verify with readelf, not by reading this.
"%CMAKE_BIN%" -S "%LLAMA_SRC%" -B "%BUILD%" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA_BIN%" ^
  -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" ^
  -DANDROID_ABI=%ABI% ^
  -DANDROID_PLATFORM=android-26 ^
  -DANDROID_STL=c++_shared ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" ^
  -DCMAKE_MODULE_LINKER_FLAGS="-Wl,-z,max-page-size=16384" ^
  -DBUILD_SHARED_LIBS=ON ^
  -DGGML_BACKEND_DL=ON ^
  -DGGML_CPU_ALL_VARIANTS=ON ^
  -DGGML_NATIVE=OFF ^
  -DGGML_OPENMP=OFF ^
  -DLLAMA_CURL=OFF ^
  -DLLAMA_BUILD_TESTS=OFF ^
  -DLLAMA_BUILD_EXAMPLES=OFF ^
  -DLLAMA_BUILD_SERVER=OFF ^
  -DLLAMA_BUILD_TOOLS=OFF
if errorlevel 1 exit /b 1

rem The `llama` target pulls ggml, ggml-base and every ggml-cpu-* variant with it.
"%CMAKE_BIN%" --build "%BUILD%" --target llama
if errorlevel 1 exit /b 1

echo [build-llama] assembling %LLAMA_SDK_DIR%
if exist "%LLAMA_SDK_DIR%" rmdir /s /q "%LLAMA_SDK_DIR%"
mkdir "%LLAMA_SDK_DIR%\include"
mkdir "%LLAMA_SDK_DIR%\lib"
copy /y "%LLAMA_SRC%\include\llama.h" "%LLAMA_SDK_DIR%\include\" >nul
copy /y "%LLAMA_SRC%\ggml\include\*.h" "%LLAMA_SDK_DIR%\include\" >nul

rem Stripped on the way in: unstripped these are 79 MB against 9.8 MB, and every one of
rem them is packaged into the APK as-is (jniLibs are not run through AGP's stripper the
rem way an externalNativeBuild output is).
set "STRIP=%MIROBODY_NDK_PATH%\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
for %%F in ("%BUILD%\bin\*.so") do (
    if exist "%STRIP%" (
        "%STRIP%" --strip-unneeded -o "%LLAMA_SDK_DIR%\lib\%%~nxF" "%%F"
    ) else (
        copy /y "%%F" "%LLAMA_SDK_DIR%\lib\" >nul
    )
)

echo [build-llama] done: %LLAMA_SDK_DIR%
echo [build-llama] the app picks this up by itself -- rebuild the APK to link it in.
exit /b 0
