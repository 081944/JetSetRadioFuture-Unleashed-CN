param(
    [string]$AndroidSdk = "$env:LOCALAPPDATA\Android\Sdk",
    [string]$JavaDir = 'C:\Program Files\Android\Android Studio\jbr',
    [string]$Version = '0.5.10-cn.2',
    [int]$VersionCode = 5,
    [string]$SigningDir = '',
    [string]$OutputDir = ''
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$OutputDir) { $OutputDir = Join-Path $repoRoot 'build-cn-apk' }
if (!$SigningDir) { $SigningDir = Join-Path $repoRoot 'build-cn-apk' }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$work = Join-Path $OutputDir ('work-' + [guid]::NewGuid().ToString('N'))
$native = Join-Path $repoRoot 'build-cn-native'
$profile = Get-Content -LiteralPath "$repoRoot/game_files/cn_compat/manifest.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$receipt = Get-Content -LiteralPath "$native/engine/cn-build.json" -Raw -Encoding UTF8 | ConvertFrom-Json
if ($receipt.version -ne $Version) { throw 'APK and native engine versions differ. Build the native library with the same -Version first.' }
if ($profile.target_sha256 -ne $receipt.target_sha256 -or
    (Get-FileHash -LiteralPath "$native/engine/libmain.so" -Algorithm SHA256).Hash.ToLowerInvariant() -ne $receipt.library_sha256) {
    throw 'Native library receipt does not match current CN profile. Run build_native.ps1 first.'
}
$bt = Join-Path $AndroidSdk 'build-tools/36.0.0'
$androidJar = Join-Path $AndroidSdk 'platforms/android-36/android.jar'
$java = Join-Path $JavaDir 'bin/java.exe'
$ndk = Get-ChildItem -LiteralPath "$AndroidSdk/ndk" -Directory |
    Where-Object { Test-Path "$($_.FullName)/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe" } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$ndk) { throw 'No complete NDK found' }
$strip = "$($ndk.FullName)/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
function Invoke-Tool {
    param([string]$Exe, [string[]]$ToolArgs)
    & $Exe @ToolArgs
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed ($LASTEXITCODE)" }
}
New-Item -ItemType Directory -Path $work -Force | Out-Null
Copy-Item -LiteralPath "$repoRoot/launcher/android/res" -Destination "$work/res" -Recurse
$stringsFile = "$work/res/values/strings.xml"
$stringsXml = [xml](Get-Content -LiteralPath $stringsFile -Raw -Encoding UTF8)
$stringsXml.SelectSingleNode('/resources/string[@name="supported_disc"]').InnerText = 'the supported Chinese-patched Japanese disc (JSRF-CN.iso)'
$stringsXml.SelectSingleNode('/resources/string[@name="app_name"]').InnerText = 'JSRF Unleashed CN'
$stringsXml.OuterXml | Set-Content -LiteralPath $stringsFile -Encoding UTF8
foreach ($density in @('mdpi', 'hdpi', 'xhdpi', 'xxhdpi', 'xxxhdpi')) {
    New-Item -ItemType Directory -Path "$work/res/mipmap-$density" -Force | Out-Null
    Copy-Item -LiteralPath "$repoRoot/launcher/icon/AppIcon.iconset/icon_256x256.png" -Destination "$work/res/mipmap-$density/ic_launcher.png"
}
foreach ($dir in @('assets/licences', 'gen', 'classes', 'dex', 'lib/arm64-v8a')) {
    New-Item -ItemType Directory -Path "$work/$dir" -Force | Out-Null
}
Copy-Item -LiteralPath "$repoRoot/LICENSE" -Destination "$work/assets/licences/LICENSE.txt"
Copy-Item -LiteralPath "$repoRoot/NOTICE" -Destination "$work/assets/licences/NOTICE.txt"
Copy-Item -LiteralPath "$repoRoot/LICENSES/LGPL-2.1.txt" -Destination "$work/assets/licences/LGPL-2.1.txt"
Copy-Item -LiteralPath "$repoRoot/game_files/SDL2/LICENSE.txt" -Destination "$work/assets/licences/SDL2-LICENSE.txt"
Invoke-Tool "$bt/aapt2.exe" @('compile', '--dir', "$work/res", '-o', "$work/res.zip")
Invoke-Tool "$bt/aapt2.exe" @('link', '-o', "$work/base.apk", '-I', $androidJar,
    '--manifest', "$repoRoot/launcher/android/AndroidManifest.xml", '--min-sdk-version', '29',
    '--target-sdk-version', '36', '--version-code', "$VersionCode", '--version-name', $Version,
    '-A', "$work/assets", '--java', "$work/gen", "$work/res.zip")
$javaSources = Get-ChildItem -Path @("$repoRoot/game_files/SDL2/android-project/app/src/main/java", "$repoRoot/launcher/android/java", "$work/gen") -Filter '*.java' -Recurse -File
$javaSources | ForEach-Object { '"' + $_.FullName.Replace('\', '/') + '"' } |
    Set-Content -LiteralPath "$work/sources.txt" -Encoding UTF8
Invoke-Tool "$JavaDir/bin/javac.exe" @('-source', '11', '-target', '11', '-nowarn', '-Xlint:-options',
    '-encoding', 'UTF-8', '-cp', $androidJar, '-d', "$work/classes", "@$work/sources.txt")
Get-ChildItem -LiteralPath "$work/classes" -Filter '*.class' -Recurse -File |
    ForEach-Object { $_.FullName.Replace('\', '/') } |
    Set-Content -LiteralPath "$work/classes.txt" -Encoding UTF8
# Call Java entry points directly; do not invoke the SDK's .bat wrappers.
Invoke-Tool $java @('-cp', "$bt/lib/d8.jar", 'com.android.tools.r8.D8', '--release', '--min-api', '29',
    '--lib', $androidJar, '--output', "$work/dex", "@$work/classes.txt")
Copy-Item -LiteralPath "$native/engine/libmain.so" -Destination "$work/lib/arm64-v8a/libmain.so"
Copy-Item -LiteralPath "$native/sdl-install/lib/libSDL2.so" -Destination "$work/lib/arm64-v8a/libSDL2.so"
Invoke-Tool $strip @('--strip-unneeded', "$work/lib/arm64-v8a/libmain.so", "$work/lib/arm64-v8a/libSDL2.so")
Copy-Item -LiteralPath "$work/base.apk" -Destination "$work/unsigned.apk"
$archive = [IO.Compression.ZipFile]::Open("$work/unsigned.apk", [IO.Compression.ZipArchiveMode]::Update)
try {
    foreach ($name in @('dex/classes.dex', 'lib/arm64-v8a/libmain.so', 'lib/arm64-v8a/libSDL2.so')) {
        $entry = if ($name -eq 'dex/classes.dex') { 'classes.dex' } else { $name }
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, "$work/$name", $entry,
            [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $archive.Dispose() }
Invoke-Tool "$bt/zipalign.exe" @('-P', '16', '-f', '4', "$work/unsigned.apk", "$work/aligned.apk")
# Persist the local test identity so later test APKs can update this install.
New-Item -ItemType Directory -Path $SigningDir -Force | Out-Null
$keystore = Join-Path $SigningDir 'cn-test.keystore'
$pass = Join-Path $SigningDir 'cn-test.keystore.pass'
if (!(Test-Path -LiteralPath $keystore)) {
    if (Test-Path -LiteralPath $pass) { throw 'Password exists without keystore; inspect the test signing files' }
    [Convert]::ToBase64String([Security.Cryptography.RandomNumberGenerator]::GetBytes(24)) |
        Set-Content -LiteralPath $pass -Encoding UTF8 -NoNewline
    Invoke-Tool "$JavaDir/bin/keytool.exe" @('-genkeypair', '-keystore', $keystore, '-alias', 'jsrf',
        '-keyalg', 'RSA', '-keysize', '4096', '-validity', '36500', '-dname', 'CN=JSRF Unleashed CN Test',
        '-storepass:file', $pass, '-keypass:file', $pass)
}
$apk = Join-Path $OutputDir 'JSRF-Unleashed-android-arm64-CN.apk'
Invoke-Tool $java @('-jar', "$bt/lib/apksigner.jar", 'sign', '--ks', $keystore, '--ks-key-alias', 'jsrf',
    '--ks-pass', "file:$pass", '--out', $apk, "$work/aligned.apk")
Invoke-Tool $java @('-jar', "$bt/lib/apksigner.jar", 'verify', '--verbose', $apk)
Invoke-Tool "$bt/zipalign.exe" @('-c', '-P', '16', '4', $apk)
Get-FileHash -LiteralPath $apk -Algorithm SHA256
Write-Output "Packaged test APK: $apk"
