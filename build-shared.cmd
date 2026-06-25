@echo off
setlocal EnableExtensions

if /I "%1"=="/?"      goto :usage
if /I "%1"=="-h"      goto :usage
if /I "%1"=="--help"  goto :usage
if /I "%1"=="help"    goto :usage

:: Build libmirobody, the C-ABI shared library (mirobody.dll / libmirobody.so)
:: consumed by FFI hosts (Java, Go, C#, Node, ...). Defaults to the SQLITE backend
:: so the library is self-contained; override MIROBODY_DATABASE_BACKEND to link a
:: different client.

set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%build-shared"

:: Shared toolchain bring-up: VS_DIR/NINJA defaults, VS_ARCH, VCPKG_ARCH (triplet
:: arch stem), vcvars, VCPKG_ROOT. Same env vars as build.cmd.
call "%PROJECT_DIR%_build_env.cmd"
if errorlevel 1 exit /b 1

:: Fully-static triplet by default: native deps AND the MSVC runtime are linked
:: statically (/MT), so mirobody.dll / mirobody_jni.dll depend on no loose vcpkg
:: DLLs and no shared vcruntime/ucrtbase. That self-containment is what makes the
:: JNI shim immune to the JDK's bundled (shadowing) CRT (see bindings/README.md).
:: These server DLLs don't pass CRT objects across the FFI boundary, so the static
:: CRT is safe (unlike a CPython extension, which build-python.cmd builds
:: -static-md to keep /MD). Override VCPKG_TRIPLET to change.
if not defined VCPKG_TRIPLET set "VCPKG_TRIPLET=%VCPKG_ARCH%-windows-static"

:: Match our code's CRT linkage to the triplet: /MT for a fully-static triplet,
:: /MD otherwise. CMAKE_MSVC_RUNTIME_LIBRARY needs policy CMP0091 (we require CMake
:: 3.19, so it is NEW). findstr /E "-static" matches x64-windows-static but not
:: x64-windows-static-md (dynamic CRT) or the default dynamic triplet.
set "_CRT=MultiThreadedDLL"
echo %VCPKG_TRIPLET%| findstr /E /C:"-static" >nul && set "_CRT=MultiThreaded"

if not defined MIROBODY_DATABASE_BACKEND set "MIROBODY_DATABASE_BACKEND=SQLITE"

:: clean: drop CMake's configuration but KEEP build-shared\vcpkg_installed, so the
:: reconfigure below picks up changed options without the slow vcpkg dependency
:: rebuild. Also fixes stale absolute paths in CMakeCache.txt after a repo move.
if /I "%1"=="clean" (
    if exist "%BUILD_DIR%" (
        for /d %%D in ("%BUILD_DIR%\*") do if /I not "%%~nxD"=="vcpkg_installed" rd /s /q "%%D"
        del /q "%BUILD_DIR%\*" 2>nul
    )
)

:: Optional shared vcpkg binary cache (see build.cmd / README).
if defined MIROBODY_VCPKG_CACHE (
    if not exist "%MIROBODY_VCPKG_CACHE%" mkdir "%MIROBODY_VCPKG_CACHE%"
    set "VCPKG_DEFAULT_BINARY_CACHE=%MIROBODY_VCPKG_CACHE%"
)

:: Build the JNI shim too when a JDK is available (find_package(JNI) uses JAVA_HOME).
set "_JNI_ARG=-DMIROBODY_BUILD_JNI=OFF"
if defined JAVA_HOME set "_JNI_ARG=-DMIROBODY_BUILD_JNI=ON"

if not exist "%BUILD_DIR%\build.ninja" (
    cmake -S "%PROJECT_DIR%." -B "%BUILD_DIR%" ^
        -G Ninja ^
        -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
        -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
        -DVCPKG_TARGET_TRIPLET=%VCPKG_TRIPLET% ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=%_CRT% ^
        -DMIROBODY_DATABASE_BACKEND=%MIROBODY_DATABASE_BACKEND% ^
        -DMIROBODY_BUILD_SHARED=ON ^
        %_JNI_ARG% ^
        -DMIROBODY_BUILD_TOOLS=OFF ^
        -DMIROBODY_BUILD_TESTS=OFF
    if errorlevel 1 exit /b 1
)

cmake --build "%BUILD_DIR%" --config Release --target mirobody_shared
if errorlevel 1 exit /b 1
if defined JAVA_HOME cmake --build "%BUILD_DIR%" --config Release --target mirobody_jni
exit /b %errorlevel%

:usage
echo Usage: build-shared.cmd [clean^|help]
echo.
echo   clean       reconfigure from scratch (keeps build-shared\vcpkg_installed)
echo   help / -h / --help / /?   show this help
echo.
echo   Builds libmirobody (the C-ABI shared library) into build-shared\, plus the
echo   JNI shim when JAVA_HOME is set. Defaults to the x64-windows-static triplet
echo   and the SQLITE backend; set VCPKG_TRIPLET or MIROBODY_DATABASE_BACKEND to
echo   override. Shares the toolchain bring-up and env vars with build.cmd
echo   (VS_DIR / VCPKG_ROOT / NINJA / MIROBODY_VCPKG_CACHE).
exit /b 0
