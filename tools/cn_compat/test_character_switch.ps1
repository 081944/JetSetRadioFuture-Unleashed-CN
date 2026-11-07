param(
    [switch]$OnDevice,
    [string]$Serial = '',
    [string]$AndroidSdk = "$env:LOCALAPPDATA/Android/Sdk"
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$output = Join-Path $repoRoot 'build/logs/character-switch'
New-Item -ItemType Directory -Force $output | Out-Null
$cases = @('roundtrip','locked','unsafe','load_timeout','scene_change','restore',
    'latest','incomplete','same','pending_after_commit','delayed_destructor','wrong_bindings','sweep_failure')
$sources = @("$PSScriptRoot/test_character_switch.c", "$repoRoot/port/android/character_switch.c")
$flags = @('-std=c11','-DJSRF_CHARACTER_TEST','-DJSRF_CN_PROFILE','-Wall','-Wextra','-Werror')
if ($OnDevice) {
    $ndkChoice = Get-ChildItem -LiteralPath "$AndroidSdk/ndk" -Directory |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    $compiler = "$($ndkChoice.FullName)/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
    $binary = "$output/test-character-switch-arm64"
    & $compiler '--target=aarch64-linux-android29' @flags @sources '-o' $binary
} else {
    $binary = "$output/test-character-switch.exe"
    & gcc @flags @sources '-o' $binary
}
if ($LASTEXITCODE -ne 0) { throw 'Character switch test compilation failed' }
if ($OnDevice) {
    $adbExe = "$AndroidSdk/platform-tools/adb.exe"
    $device = @()
    if ($Serial) { $device = @('-s',$Serial) }
    & $adbExe @device push $binary '/data/local/tmp/jsrf-character-switch-test'
    if ($LASTEXITCODE -ne 0) { throw 'Test executable transfer failed' }
    & $adbExe @device shell chmod 755 '/data/local/tmp/jsrf-character-switch-test'
    if ($LASTEXITCODE -ne 0) { throw 'Test executable setup failed' }
}
foreach ($case in $cases) {
    if ($OnDevice) { & $adbExe @device shell '/data/local/tmp/jsrf-character-switch-test' $case }
    else { & $binary $case }
    if ($LASTEXITCODE -ne 0) { throw "Failed character switch scenario: $case" }
}
Write-Output "PASS: $($cases.Count) production state machine scenarios (guest calls simulated)"
