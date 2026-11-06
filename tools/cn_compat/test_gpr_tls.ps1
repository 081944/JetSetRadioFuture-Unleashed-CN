param(
    [string]$AndroidSdk = "$env:LOCALAPPDATA/Android/Sdk",
    [string]$Serial = '',
    [string]$OutputDir = ''
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$OutputDir) { $OutputDir = Join-Path $repoRoot 'build/logs/gpr-tls-test' }
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$ndk = Get-ChildItem "$AndroidSdk/ndk" -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$clangExe = "$($ndk.FullName)/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
$adbExe = "$AndroidSdk/platform-tools/adb.exe"
function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Native tool failed: $Exe ($LASTEXITCODE)" }
}
$types = Get-Content "$repoRoot/port/recomp/gen/recomp_types.h" -Encoding UTF8 -Raw
$anchor = 'extern RECOMP_TLS uint32_t g_ebp;'
if ([regex]::Matches($types,[regex]::Escape($anchor)).Count -ne 1) { throw 'Missing unique GPR declaration hook' }
$types.Replace($anchor, $anchor + "`n#include `"$repoRoot/port/recomp_gpr_aliases.h`"") |
    Set-Content "$OutputDir/recomp_gpr_types.h" -Encoding UTF8
$common = @('--target=aarch64-linux-android29','-O2','-g1','-fPIC',"-I$repoRoot/port/recomp/gen","-I$repoRoot/port","-I$OutputDir")
$payload = Join-Path $PSScriptRoot 'test_gpr_tls_payload.c'
Invoke-Native $clangExe ($common + @('-DGPR_LEGACY','-c',$payload,'-o',"$OutputDir/legacy.o"))
Invoke-Native $clangExe ($common + @('-DJSRF_GROUPED_GPR_TLS=1','-DJSRF_GPR_GENERATED=1','-c',$payload,'-o',"$OutputDir/grouped.o"))
Invoke-Native $clangExe ($common + @('-shared',"$repoRoot/src/kernel/recomp_gpr.c","$OutputDir/legacy.o","$OutputDir/grouped.o",'-o',"$OutputDir/libgpr-test.so"))
Invoke-Native $clangExe ($common + @("$PSScriptRoot/test_gpr_tls_runner.c",'-ldl','-o',"$OutputDir/gpr-test"))
$device = @()
if ($Serial) { $device = @('-s',$Serial) }
Invoke-Native $adbExe ($device + @('push',"$OutputDir/libgpr-test.so",'/data/local/tmp/jsrf-libgpr-test.so'))
Invoke-Native $adbExe ($device + @('push',"$OutputDir/gpr-test",'/data/local/tmp/jsrf-gpr-test'))
Invoke-Native $adbExe ($device + @('shell','chmod','755','/data/local/tmp/jsrf-gpr-test'))
foreach ($run in 1..3) {
    $result = & $adbExe @device shell /data/local/tmp/jsrf-gpr-test /data/local/tmp/jsrf-libgpr-test.so
    if ($LASTEXITCODE -ne 0) { throw 'GPR TLS device validation failed' }
    $result | Set-Content "$OutputDir/run-$run.log" -Encoding UTF8
    $result
}
