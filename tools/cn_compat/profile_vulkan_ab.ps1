param(
    [string]$Adb = 'D:/Softwares/platform-tools/adb.exe',
    [string]$OutputDir = '',
    [ValidateSet('Optimization', 'Backends')]
    [string]$Comparison = 'Optimization',
    [int]$WarmupSeconds = 60,
    [int]$SampleSeconds = 15
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$OutputDir) { $OutputDir = Join-Path $repoRoot 'build/profiles/vulkan-ab' }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'build-cn-native/engine/libmain.so') -Destination (Join-Path $OutputDir 'libmain.so')
function Invoke-Adb {
    param([string[]]$AdbArgs)
    $result = & $Adb @AdbArgs
    if ($LASTEXITCODE -ne 0) { throw "adb failed: $($AdbArgs -join ' ')" }
    return $result
}
$cases = @(
    @{ Name='spin'; Wait=0; Dedup=0 },
    @{ Name='blocking'; Wait=1; Dedup=0 },
    @{ Name='dedup'; Wait=1; Dedup=1 }
)
if ($Comparison -eq 'Backends') {
    $cases = @(
        @{ Name='gl'; Backend='gl'; Wait=1; Dedup=1 },
        @{ Name='vulkan'; Backend='vulkan'; Wait=1; Dedup=1 }
    )
}
Invoke-Adb @('shell', "setprop debug.jsrf.recomp_backend vulkan; setprop debug.jsrf.recomp_gl_profile ''; setprop debug.jsrf.recomp_frame_stats 1")
foreach ($case in $cases) {
    $name = $case.Name
    $backend = if ($case.Backend) { $case.Backend } else { 'vulkan' }
    Invoke-Adb @('shell','input','keyevent','KEYCODE_WAKEUP') | Out-Null
    Invoke-Adb @('shell','wm','dismiss-keyguard') | Out-Null
    Invoke-Adb @('shell', "setprop debug.jsrf.recomp_backend $backend; setprop debug.jsrf.recomp_nv2a_flush_wait $($case.Wait); setprop debug.jsrf.recomp_vk_uniform_dedup $($case.Dedup); am force-stop com.jumatt.jsrfunleashed; monkey -p com.jumatt.jsrfunleashed -c android.intent.category.LAUNCHER 1") | Out-Null
    Write-Output "Warming $name for $WarmupSeconds seconds"
    Start-Sleep -Seconds $WarmupSeconds
    $powerState = (Invoke-Adb @('shell','dumpsys','power')) -join "`n"
    if ($powerState -notmatch 'mWakefulness=Awake') { throw "Device not awake for $name; discard this run" }
    $appPid = (Invoke-Adb @('shell','pidof','com.jumatt.jsrfunleashed')).Trim()
    if ($appPid -notmatch '^\d+$') { throw "Missing app PID for $name" }
    Invoke-Adb @('shell', "su -c 'simpleperf record -p $appPid -e cpu-clock -f 1000 --call-graph dwarf,8192 --duration $SampleSeconds -o /data/local/tmp/jsrf-ab-$name.data'")
    Invoke-Adb @('shell', "su -c 'chmod 644 /data/local/tmp/jsrf-ab-$name.data'")
    Invoke-Adb @('pull', "/data/local/tmp/jsrf-ab-$name.data", (Join-Path $OutputDir "$name.data"))
    Invoke-Adb @('logcat','-d','--pid',$appPid) | Set-Content (Join-Path $OutputDir "$name.log")
    $frameLines = Get-Content (Join-Path $OutputDir "$name.log") | Select-String '\[FRAMESTATS\]'
    if (!$frameLines) { throw "No frame statistics for $name; discard this run" }
    Invoke-Adb @('shell',"su -c 'dumpsys battery; cat /sys/class/kgsl/kgsl-3d0/gpubusy; cat /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq'") | Set-Content (Join-Path $OutputDir "$name-device.txt")
    Invoke-Adb @('shell','screencap','-p',"/sdcard/jsrf-ab-$name.png")
    Invoke-Adb @('pull',"/sdcard/jsrf-ab-$name.png",(Join-Path $OutputDir "$name.png"))
    Get-Content (Join-Path $OutputDir "$name.log") | Select-String '\[FRAMESTATS\]|uniform counters:' | Select-Object -Last 12
}
