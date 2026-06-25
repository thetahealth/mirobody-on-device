<#
.SYNOPSIS
    Cross-compile the C++ server's native dependencies for Android and lay them
    out under android/prebuilt/<ABI>/ so that app/build.gradle.kts (which passes
    -DCMAKE_PREFIX_PATH=android/prebuilt/${ANDROID_ABI}) and the repo-root
    CMakeLists.txt find_package() calls resolve.

.DESCRIPTION
    Uses vcpkg's Android triplets (arm64-android, etc.), pinned to the same
    builtin-baseline as vcpkg.json, to build exactly the libraries the server
    links: openssl, curl (openssl backend), libwebsockets, yaml-cpp, hiredis,
    rapidjson, sqlite3. libpq / openblas / catch2 are desktop/test-only and are
    intentionally omitted (mobile uses the SQLite backend; tests are disabled on
    Android in CMakeLists.txt).

    The result is copied into android/prebuilt/<ABI>/ (git-ignored). Re-running is
    idempotent: vcpkg skips already-built ports and the destination is refreshed.

.EXAMPLE
    powershell -File android/build-prebuilt.ps1
    powershell -File android/build-prebuilt.ps1 -Abi arm64-v8a -VcpkgRoot D:\projects\vcpkg
#>
[CmdletBinding()]
param(
    [ValidateSet("arm64-v8a", "armeabi-v7a", "x86_64", "x86")]
    [string]$Abi = "arm64-v8a",
    [string]$VcpkgRoot = "",
    [string]$NdkHome = $env:ANDROID_NDK_HOME
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path $PSScriptRoot -Parent

# Keep the pin in sync with vcpkg.json's "builtin-baseline".
$baseline = "9b965a116838c6cdcd36bca60d1b81b030c8ab8d"

# Android ABI -> vcpkg triplet.
$triplet = switch ($Abi) {
    "arm64-v8a"   { "arm64-android" }
    "armeabi-v7a" { "arm-android" }
    "x86_64"      { "x64-android" }
    "x86"         { "x86-android" }
}

# Default the vcpkg checkout to a sibling of the repo so it is not committed.
if (-not $VcpkgRoot) { $VcpkgRoot = Join-Path (Split-Path $repoRoot -Parent) "vcpkg" }

# --- Resolve the NDK -----------------------------------------------------------
# Must match the ndkVersion pinned in app/build.gradle.kts so the prebuilt deps and
# the app share one libc++ (c++_shared) ABI; prefer that exact version, else newest.
$pinnedNdk = "27.0.12077973"
if (-not $NdkHome -or -not (Test-Path $NdkHome)) {
    $sdk = $env:ANDROID_HOME
    if (-not $sdk) { $sdk = Join-Path $env:LOCALAPPDATA "Android\Sdk" }
    $ndkDir = Join-Path $sdk "ndk"
    if (Test-Path (Join-Path $ndkDir $pinnedNdk)) {
        $NdkHome = Join-Path $ndkDir $pinnedNdk
    } elseif (Test-Path $ndkDir) {
        $NdkHome = (Get-ChildItem $ndkDir -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
    }
}
if (-not $NdkHome -or -not (Test-Path (Join-Path $NdkHome "build\cmake\android.toolchain.cmake"))) {
    throw "Android NDK not found. Pass -NdkHome or set ANDROID_NDK_HOME."
}
$env:ANDROID_NDK_HOME = $NdkHome
Write-Host "ABI=$Abi  triplet=$triplet"
Write-Host "NDK=$NdkHome"
Write-Host "vcpkg=$VcpkgRoot"

# --- Bootstrap vcpkg at the pinned baseline ------------------------------------
if (-not (Test-Path (Join-Path $VcpkgRoot ".git"))) {
    Write-Host "Cloning vcpkg ..."
    git clone https://github.com/microsoft/vcpkg "$VcpkgRoot"
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
}
git -C "$VcpkgRoot" checkout $baseline
if ($LASTEXITCODE -ne 0) { throw "git checkout $baseline failed" }
if (-not (Test-Path (Join-Path $VcpkgRoot "vcpkg.exe"))) {
    & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "bootstrap-vcpkg failed" }
}

# --- Build the server's deps for Android (classic mode) ------------------------
$vcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
$ports = @("openssl", "curl[openssl]", "libwebsockets", "yaml-cpp", "hiredis", "rapidjson", "sqlite3")
Write-Host "Installing: $($ports -join ', ')  (--triplet $triplet)"
# --classic: this script is run from inside the repo, whose vcpkg.json would otherwise put
# vcpkg into manifest mode (which rejects port-name arguments). Force classic mode.
& $vcpkgExe install @ports --classic --triplet $triplet --x-buildtrees-root (Join-Path $VcpkgRoot "buildtrees")
if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed" }

# --- Lay out under android/prebuilt/<ABI> --------------------------------------
$installed = Join-Path $VcpkgRoot "installed\$triplet"
if (-not (Test-Path $installed)) { throw "expected install tree missing: $installed" }
$dest = Join-Path $PSScriptRoot "prebuilt\$Abi"
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
New-Item -ItemType Directory -Force $dest | Out-Null
# Copy debug/ too: vcpkg's *Targets.cmake reference both release (lib/) and debug
# (debug/lib/) imported locations and CMake verifies both exist at find_package time.
foreach ($sub in @("include", "lib", "share", "debug")) {
    $from = Join-Path $installed $sub
    if (Test-Path $from) {
        robocopy $from (Join-Path $dest $sub) /E /NFL /NDL /NJH /NJS | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "robocopy $sub failed ($LASTEXITCODE)" }
    }
}
# robocopy returns non-zero (1-7) on success; clear it so the script exits 0.
$global:LASTEXITCODE = 0
Write-Host ""
Write-Host "Done. Prebuilt deps for $Abi -> $dest"
Write-Host "Now build the app (Android Studio) or the native target directly:"
Write-Host "  cmake -B build-android -G Ninja \"
Write-Host "    -DCMAKE_TOOLCHAIN_FILE=$NdkHome\build\cmake\android.toolchain.cmake \"
Write-Host "    -DANDROID_ABI=$Abi -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared \"
Write-Host "    -DCMAKE_FIND_ROOT_PATH=$dest -DCMAKE_PREFIX_PATH=$dest -DMIROBODY_DATABASE_BACKEND=SQLITE \"
Write-Host "    -DMIROBODY_BUILD_TOOLS=OFF -DMIROBODY_BUILD_TESTS=OFF"
Write-Host "  cmake --build build-android --target mirobody"
