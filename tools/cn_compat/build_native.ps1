param(
    [string]$AndroidSdk = "$env:LOCALAPPDATA\Android\Sdk",
    [string]$SdlSource = '',
    [string]$CnProfileDir = '',
    [string]$GeneratedDir = '',
    [string]$Version = '',
    [int]$VersionCode = 0,
    [int]$Jobs = 4,
    [switch]$AllowVersionOverride,
    [switch]$SdlOnly
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'version.ps1')
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$release = Resolve-CnReleaseInfo -Info (Get-CnReleaseInfo) -Version $Version -VersionCode $VersionCode -AllowOverride:$AllowVersionOverride
if ($release.overridden) {
    Write-Warning "Using overridden version $($release.version) / versionCode $($release.versionCode); $($release.source) says otherwise."
}
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
$profileManifest = Get-Content -LiteralPath (Join-Path $CnProfileDir 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
# Refuse to link generated code that was produced from a different CN profile.
$genStampPath = Join-Path $GeneratedDir 'cn_codegen.sha256'
if (!(Test-Path -LiteralPath $genStampPath)) {
    throw "Generated code stamp missing: $genStampPath. Run generate.py against $CnProfileDir first."
}
$genStamp = (Get-Content -LiteralPath $genStampPath -Raw -Encoding UTF8).Trim()
if ($genStamp -ne $profileManifest.target_sha256) {
    throw ("Generated code was built from $genStamp but the CN profile targets $($profileManifest.target_sha256). " +
        'Re-run generate.py against the current profile.')
}
Invoke-CMake -CMakeArgs @('-S', "$repoRoot/port", '-B', "$workDir/engine", '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninjaPath", "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29',
    '-DCMAKE_BUILD_TYPE=RelWithDebInfo', "-DJSRF_VERSION=$($release.version)",
    "-DJSRF_GEN_DIR=$GeneratedDir", "-DJSRF_CN_PROFILE_DIR=$CnProfileDir",
    "-DSDL2_DIR=$sdlInstall/lib/cmake/SDL2")
Invoke-CMake -CMakeArgs @('--build', "$workDir/engine", '-j', "$Jobs", '--target', 'jsrf_recomp')
$source = Get-CnGitState -RepoRoot $repoRoot
$nativeReceipt = [ordered]@{
    version         = $release.version
    versionCode     = $release.versionCode
    target_sha256   = $profileManifest.target_sha256
    gen_stamp       = $genStamp
    library_sha256  = (Get-FileHash -LiteralPath "$workDir/engine/libmain.so" -Algorithm SHA256).Hash.ToLowerInvariant()
    source_commit   = $source.commit
    source_dirty    = $source.dirty
    ndk             = $ndkChoice.Name
    built_at        = (Get-Date).ToString('o')
}
Write-CnJsonFile -Path "$workDir/engine/cn-build.json" -Object $nativeReceipt
Write-Output "Native compilation complete: $workDir/engine/libmain.so (no APK packaged)"
Write-Output "Receipt: $(Join-Path $workDir 'engine/cn-build.json')"
