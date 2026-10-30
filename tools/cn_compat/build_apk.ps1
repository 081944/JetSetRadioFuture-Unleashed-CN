param(
    [string]$AndroidSdk = "$env:LOCALAPPDATA\Android\Sdk",
    [string]$JavaDir = 'C:\Program Files\Android\Android Studio\jbr',
    [string]$Version = '',
    [int]$VersionCode = 0,
    [string]$SigningDir = '',
    [string]$OutputDir = '',
    [switch]$AllowVersionOverride,
    [switch]$AllowNewIdentity
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'version.ps1')
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$OutputDir) { $OutputDir = Join-Path $repoRoot 'build-cn-apk' }
if (!$SigningDir) { $SigningDir = Join-Path $repoRoot 'build-cn-apk' }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$work = Join-Path $OutputDir ('work-' + [guid]::NewGuid().ToString('N'))
$native = Join-Path $repoRoot 'build-cn-native'

$release = Resolve-CnReleaseInfo -Info (Get-CnReleaseInfo) -Version $Version -VersionCode $VersionCode -AllowOverride:$AllowVersionOverride
if ($release.overridden) {
    Write-Warning "Using overridden version $($release.version) / versionCode $($release.versionCode); $($release.source) says otherwise."
}
$source = Get-CnGitState -RepoRoot $repoRoot

# Refuse to republish a released version number from divergent content. A later
# documentation commit is fine: the tag stays on the build commit by convention.
$tagName = "v$($release.version)"
$tagCommit = (& git -C $repoRoot rev-list -n 1 $tagName 2>$null)
if ($LASTEXITCODE -eq 0 -and $tagCommit) {
    & git -C $repoRoot merge-base --is-ancestor $tagCommit $source.commit 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw ("$tagName points at $tagCommit, which is not an ancestor of HEAD ($($source.commit)). " +
            'Bump version.json instead of shipping a second package under a released version.')
    }
    Write-Warning "$tagName is already released; this build is a rebuild of that version, not a new release."
}

$profile = Get-Content -LiteralPath "$repoRoot/game_files/cn_compat/manifest.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$receipt = Get-Content -LiteralPath "$native/engine/cn-build.json" -Raw -Encoding UTF8 | ConvertFrom-Json
if ($receipt.version -ne $release.version) {
    throw "APK and native engine versions differ ($($release.version) vs $($receipt.version)). Build the native library with the same version first."
}
if (!$receipt.gen_stamp) {
    throw 'Native receipt predates the version/gen checks. Re-run build_native.ps1 to refresh build-cn-native/engine/cn-build.json.'
}
if ($receipt.target_sha256 -ne $profile.target_sha256) {
    throw 'Native library was built from a different CN profile than game_files/cn_compat. Re-run build_native.ps1.'
}
if ($receipt.gen_stamp -ne $profile.target_sha256) {
    throw 'Generated code and CN profile disagree; re-run generate.py and build_native.ps1.'
}
if ((Get-FileHash -LiteralPath "$native/engine/libmain.so" -Algorithm SHA256).Hash.ToLowerInvariant() -ne $receipt.library_sha256) {
    throw 'libmain.so does not match its build receipt. Re-run build_native.ps1.'
}

$buildToolsVersion = '36.0.0'
$platformVersion = 'android-36'
$bt = Join-Path $AndroidSdk "build-tools/$buildToolsVersion"
$androidJar = Join-Path $AndroidSdk "platforms/$platformVersion/android.jar"
$java = Join-Path $JavaDir 'bin/java.exe'
if (!(Test-Path -LiteralPath "$bt/aapt2.exe")) {
    throw "Android build-tools $buildToolsVersion not found under $AndroidSdk. Install with: sdkmanager `"build-tools;$buildToolsVersion`""
}
if (!(Test-Path -LiteralPath $androidJar)) {
    throw "Android platform $platformVersion not found under $AndroidSdk. Install with: sdkmanager `"platforms;$platformVersion`""
}
if (!(Test-Path -LiteralPath $java)) { throw "Java not found: $java. Pass -JavaDir." }
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
    '--target-sdk-version', '36', '--version-code', "$($release.versionCode)", '--version-name', "$($release.version)",
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

# The signing identity is shared across machines and must not be replaced silently:
# an APK signed by a new key cannot be installed over an existing one.
$identityFile = Join-Path $PSScriptRoot 'signing.json'
if (!(Test-Path -LiteralPath $identityFile)) { throw "Release identity missing: $identityFile" }
$identity = Get-Content -LiteralPath $identityFile -Raw -Encoding UTF8 | ConvertFrom-Json
$expectedCert = ([string]$identity.cert_sha256).ToLowerInvariant()
if ($expectedCert -notmatch '^[0-9a-f]{64}$') { throw "${identityFile}: cert_sha256 must be 64 hex characters" }
$keyAlias = if ($identity.key_alias) { [string]$identity.key_alias } else { 'jsrf' }
New-Item -ItemType Directory -Path $SigningDir -Force | Out-Null
$keystore = Join-Path $SigningDir 'cn-test.keystore'
$pass = Join-Path $SigningDir 'cn-test.keystore.pass'
$newIdentity = $false
if (!(Test-Path -LiteralPath $keystore)) {
    if (Test-Path -LiteralPath $pass) { throw 'Password exists without keystore; inspect the test signing files' }
    if (!$AllowNewIdentity) {
        throw ("Signing identity missing: $keystore`n" +
            "A new key would produce an APK that cannot install over builds signed with $expectedCert. " +
            'Restore the keystore from your offline backup, or pass -AllowNewIdentity for a throwaway local build.')
    }
    [Convert]::ToBase64String([Security.Cryptography.RandomNumberGenerator]::GetBytes(24)) |
        Set-Content -LiteralPath $pass -Encoding UTF8 -NoNewline
    Invoke-Tool "$JavaDir/bin/keytool.exe" @('-genkeypair', '-keystore', $keystore, '-alias', $keyAlias,
        '-keyalg', 'RSA', '-keysize', '4096', '-validity', '36500',
        '-dname', $(if ($identity.cert_dn) { [string]$identity.cert_dn } else { 'CN=JSRF Unleashed CN Test' }),
        '-storepass:file', $pass, '-keypass:file', $pass)
    $newIdentity = $true
    Write-Warning "Created a throwaway signing identity at $keystore; it will not update existing installs."
}

$apk = Join-Path $OutputDir 'JSRF-Unleashed-android-arm64-CN.apk'
Invoke-Tool $java @('-jar', "$bt/lib/apksigner.jar", 'sign', '--ks', $keystore, '--ks-key-alias', $keyAlias,
    '--ks-pass', "file:$pass", '--out', $apk, "$work/aligned.apk")
Invoke-Tool $java @('-jar', "$bt/lib/apksigner.jar", 'verify', '--verbose', $apk)
Invoke-Tool "$bt/zipalign.exe" @('-c', '-P', '16', '4', $apk)

# Read the certificate from the signed APK itself, then compare it with signing.json.
$certOut = (& $java -jar "$bt/lib/apksigner.jar" verify --print-certs $apk 2>$null)
if ($LASTEXITCODE -ne 0) { throw 'Could not read the signing certificate' }
$certMatch = [regex]::Match((($certOut -join '') -replace '\s', ''), 'certificateSHA-256digest:([0-9a-fA-F]{64})')
if (!$certMatch.Success) { throw 'Could not parse the signing certificate digest' }
$certSha = $certMatch.Groups[1].Value.ToLowerInvariant()
if (!$newIdentity -and $certSha -ne $expectedCert) {
    throw ("APK is signed with $certSha but the release identity is $expectedCert. " +
        "Refusing to package a build that cannot update existing installs.")
}

$apkHash = (Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash.ToLowerInvariant()
$apkBytes = (Get-Item -LiteralPath $apk).Length
$apkName = [IO.Path]::GetFileName($apk)
[IO.File]::WriteAllText((Join-Path $OutputDir "$apkName.sha256"), "$apkHash  $apkName`n",
    (New-Object System.Text.UTF8Encoding($false)))

$report = [ordered]@{
    schema                        = 1
    apk                           = $apkName
    sha256                        = $apkHash
    bytes                         = $apkBytes
    version                       = $release.version
    versionCode                   = $release.versionCode
    version_source                = $release.source
    version_overridden            = [bool]$release.overridden
    source_commit                 = $source.commit
    source_branch                 = $source.branch
    source_dirty                  = $source.dirty
    profile_target_sha256         = $profile.target_sha256
    profile_localized_strings     = $profile.localized_strings
    gen_stamp                     = $receipt.gen_stamp
    native_version                = $receipt.version
    native_library_sha256         = $receipt.library_sha256
    cert_sha256                   = $certSha
    signed_with_persistent_identity = (-not $newIdentity)
    toolchain                     = [ordered]@{
        ndk         = $ndk.Name
        build_tools = $buildToolsVersion
        platform    = $platformVersion
        java_dir    = $JavaDir
    }
    built_at                      = (Get-Date).ToString('o')
    built_on                      = [ordered]@{
        host = $env:COMPUTERNAME
        user = $env:USERNAME
        os   = [Environment]::OSVersion.VersionString
    }
}
$reportPath = Join-Path $OutputDir 'build-report.json'
Write-CnJsonFile -Path $reportPath -Object $report

Get-FileHash -LiteralPath $apk -Algorithm SHA256
Write-Output "Packaged APK: $apk"
Write-Output "SHA256 sidecar: $(Join-Path $OutputDir "$apkName.sha256")"
Write-Output "Build report: $reportPath"
