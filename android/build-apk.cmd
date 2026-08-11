@echo off
rem Build the Mirobody Android APK (UI client + embedded C++ server).
rem
rem Steps: ensure the native deps are cross-compiled for the target ABI, resolve a
rem Gradle (gradlew.bat -> gradle on PATH -> download the pinned distribution), then
rem assemble the APK. The embedded server's libmirobody.so is compiled by Gradle's
rem externalNativeBuild only when android\prebuilt\<ABI>\ exists.
rem
rem Usage:  build_apk.cmd [debug^|release] [abi] [phone^|watch]
rem   abi defaults to arm64-v8a (the ABI wired in app\build.gradle.kts).
rem   flavor defaults to phone; watch targets small AOSP wearables (~410x502).
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=debug"
set "ABI=%~2"
if "%ABI%"=="" set "ABI=arm64-v8a"
set "FLAVOR=%~3"
if "%FLAVOR%"=="" set "FLAVOR=phone"
if /i "%BUILD_TYPE%"=="debug" (
    set "TYPE_CAP=Debug"
) else if /i "%BUILD_TYPE%"=="release" (
    set "TYPE_CAP=Release"
) else (
    echo build type must be 'debug' or 'release' ^(got '%BUILD_TYPE%'^)>&2
    exit /b 2
)
if /i "%FLAVOR%"=="phone" (
    set "FLAVOR_CAP=Phone"
) else if /i "%FLAVOR%"=="watch" (
    set "FLAVOR_CAP=Watch"
) else (
    echo flavor must be 'phone' or 'watch' ^(got '%FLAVOR%'^)>&2
    exit /b 2
)
set "TASK=assemble%FLAVOR_CAP%%TYPE_CAP%"

rem --- 1. Ensure prebuilt native deps ------------------------------------------
if not exist "prebuilt\%ABI%" (
    echo ==^> prebuilt\%ABI% missing; cross-compiling native deps
    call "%~dp0build-prebuilt.cmd" "%ABI%"
    if errorlevel 1 exit /b 1
)

rem --- 2. JAVA_HOME: Android Studio's JBR (JetBrains JDK 21) --------------------
rem The daemon-jvm criteria require a JetBrains-vendor JDK 21; foojay cannot
rem download one, so point Gradle at the JBR bundled with Android Studio.
if exist "%ProgramFiles%\Android\Android Studio\jbr\bin\java.exe" (
    set "JAVA_HOME=%ProgramFiles%\Android\Android Studio\jbr"
)

rem --- 3. Resolve a Gradle launcher --------------------------------------------
rem Prefer the wrapper, then the pinned distribution. We deliberately do NOT fall
rem back to `gradle` on PATH: an older launcher (e.g. 8.2.1) injects a -javaagent
rem into the build JVM that this build cannot load, so we pin the version from
rem gradle\wrapper\gradle-wrapper.properties instead.
set "GRADLE="
if exist "gradlew.bat" (
    set "GRADLE=gradlew.bat"
) else (
    rem Download the distribution pinned in gradle\wrapper\gradle-wrapper.properties.
    for /f "tokens=2 delims==" %%U in ('findstr /b distributionUrl gradle\wrapper\gradle-wrapper.properties') do set "URL=%%U"
    set "URL=!URL:\:=:!"
    rem Extract the version from ".../gradle-<VER>-bin.zip".
    set "VER=!URL:*gradle-=!"
    set "VER=!VER:-bin.zip=!"
    set "CACHE=%LOCALAPPDATA%\mirobody-gradle"
    set "BIN=!CACHE!\gradle-!VER!\bin\gradle.bat"
    if not exist "!BIN!" (
        echo ==^> No gradlew found; downloading pinned Gradle !VER!
        if not exist "!CACHE!" mkdir "!CACHE!"
        powershell -NoProfile -Command "Invoke-WebRequest -Uri '!URL!' -OutFile '!CACHE!\gradle.zip'; Expand-Archive -Path '!CACHE!\gradle.zip' -DestinationPath '!CACHE!' -Force"
        if errorlevel 1 exit /b 1
    )
    set "GRADLE=!BIN!"
)

rem --- 4. Build ----------------------------------------------------------------
echo ==^> !GRADLE! :app:%TASK% (ABI=%ABI%)
call "!GRADLE!" ":app:%TASK%" --no-daemon
if errorlevel 1 exit /b 1

rem --- 5. Report ---------------------------------------------------------------
rem Product flavours nest the output under apk\<flavor>\<type>\.
set "OUTDIR=app\build\outputs\apk\%FLAVOR%\%BUILD_TYPE%"
set "APK="
for /f "delims=" %%F in ('dir /b /s "%OUTDIR%\*.apk" 2^>nul') do set "APK=%%F"
if defined APK (
    echo APK: !APK!
) else (
    echo Build finished but no APK found under %OUTDIR%>&2
    exit /b 1
)
endlocal
