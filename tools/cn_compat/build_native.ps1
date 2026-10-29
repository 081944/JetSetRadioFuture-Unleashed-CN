param(
    [string]$AndroidSdk = "$env:LOCALAPPDATA\Android\Sdk",
    [string]$SdlSource = '',
    [string]$CnProfileDir = '',
    [string]$GeneratedDir = '',
    [string]$Version = '0.5.10-cn.1',
    [int]$Jobs = 4,
    [switch]$SdlOnly
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$SdlSource) { $SdlSource = Join-Path $repoRoot 'game_files/SDL2' }
if (!$CnProfileDir) { $CnProfileDir = Join-Path $repoRoot 'game_files/cn_compat' }
if (!$GeneratedDir) { $GeneratedDir = Join-Path $repoRoot 'port/recomp/gen' }
$ndkChoice = Get-ChildItem -LiteralPath (Join-Path $AndroidSdk 'ndk') -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'build/cmake/android.toolchain.cmake') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$ndkChoice) { throw 'No complete Android NDK installation found' }
$toolchain = Join-Path $ndkChoice.FullName 'build/cmake/android.toolchain.cmake'
$workDir = Join-Path $repoRoot 'build-cn-native'
$sdlInstall = Join-Path $workDir 'sdl-install'
$ninjaPath = (Get-Command ninja -ErrorAction Stop).Source
$sdkCmake = Get-ChildItem -LiteralPath (Join-Path $AndroidSdk 'cmake') -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'bin/cmake.exe') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$cmakePath = if ($sdkCmake) { Join-Path $sdkCmake.FullName 'bin/cmake.exe' } else { (Get-Command cmake).Source }
function Invoke-CMake {
    param([string[]]$CMakeArgs)
    & $cmakePath @CMakeArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake failed ($LASTEXITCODE)" }
}
Invoke-CMake -CMakeArgs @('-S', $SdlSource, '-B', "$workDir/sdl", '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninjaPath", "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29',
    '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DSDL_STATIC=OFF', '-DSDL_TEST=OFF',
    '-DSDL_TESTS=OFF', "-DCMAKE_INSTALL_PREFIX=$sdlInstall")
Invoke-CMake -CMakeArgs @('--build', "$workDir/sdl", '-j', "$Jobs")
Invoke-CMake -CMakeArgs @('--install', "$workDir/sdl")
if ($SdlOnly) { return }
Invoke-CMake -CMakeArgs @('-S', "$repoRoot/port", '-B', "$workDir/engine", '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninjaPath", "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29',
    '-DCMAKE_BUILD_TYPE=RelWithDebInfo', "-DJSRF_VERSION=$Version",
    "-DJSRF_GEN_DIR=$GeneratedDir", "-DJSRF_CN_PROFILE_DIR=$CnProfileDir",
    "-DSDL2_DIR=$sdlInstall/lib/cmake/SDL2")
Invoke-CMake -CMakeArgs @('--build', "$workDir/engine", '-j', "$Jobs", '--target', 'jsrf_recomp')
$profileManifest = Get-Content -LiteralPath (Join-Path $CnProfileDir 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$nativeReceipt = [ordered]@{
    version = $Version
    target_sha256 = $profileManifest.target_sha256
    library_sha256 = (Get-FileHash -LiteralPath "$workDir/engine/libmain.so" -Algorithm SHA256).Hash.ToLowerInvariant()
}
$nativeReceipt | ConvertTo-Json | Set-Content -LiteralPath "$workDir/engine/cn-build.json" -Encoding UTF8
Write-Output "Native compilation complete: $workDir/engine/libmain.so (no APK packaged)"
