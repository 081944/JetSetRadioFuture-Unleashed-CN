# Shared release metadata for the CN Android builds.
#
# version.json is the single source of truth for the version string and the
# Android versionCode. Both build scripts load it through this file, so two
# machines cannot label different content with the same version number.
#
# A different -Version is only honoured together with -AllowVersionOverride,
# which is meant for throwaway local builds and is recorded in build-report.json.

function Get-CnReleaseInfo {
    param([string]$Path)

    if (!$Path) { $Path = Join-Path $PSScriptRoot 'version.json' }
    if (!(Test-Path -LiteralPath $Path)) { throw "Release metadata missing: $Path" }

    try { $info = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json }
    catch { throw "Cannot parse ${Path}: $($_.Exception.Message)" }

    $version = [string]$info.version
    if ($version -notmatch '^\d+\.\d+\.\d+(?:-cn\.\d+)?$') {
        throw "${Path}: invalid version '$version' (expected something like 0.5.10-cn.2)"
    }
    $code = 0
    if (![int]::TryParse([string]$info.versionCode, [ref]$code) -or $code -le 0) {
        throw "${Path}: versionCode must be a positive integer"
    }
    [pscustomobject][ordered]@{
        version     = $version
        versionCode = $code
        source      = (Resolve-Path -LiteralPath $Path).Path
    }
}

function Resolve-CnReleaseInfo {
    param(
        [Parameter(Mandatory = $true)] $Info,
        [string]$Version = '',
        [int]$VersionCode = 0,
        [switch]$AllowOverride
    )

    $version = if ($Version) { $Version } else { $Info.version }
    $code = if ($VersionCode -gt 0) { $VersionCode } else { $Info.versionCode }
    if (!$AllowOverride) {
        if ($version -ne $Info.version) {
            throw ("Version '$version' does not match $($Info.source) ('$($Info.version)'). " +
                'Edit version.json instead of passing -Version, or pass -AllowVersionOverride for a local-only build.')
        }
        if ($code -ne $Info.versionCode) {
            throw ("versionCode $code does not match $($Info.source) ($($Info.versionCode)). " +
                'Edit version.json instead of passing -VersionCode, or pass -AllowVersionOverride for a local-only build.')
        }
    }
    [pscustomobject][ordered]@{
        version     = $version
        versionCode = $code
        overridden  = ($version -ne $Info.version -or $code -ne $Info.versionCode)
        source      = $Info.source
    }
}

function Get-CnGitState {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $commit = (& git -C $RepoRoot rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or !$commit) { $commit = 'unknown' }
    $branch = (& git -C $RepoRoot rev-parse --abbrev-ref HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or !$branch) { $branch = 'unknown' }
    [pscustomobject][ordered]@{
        commit = "$commit".Trim()
        branch = "$branch".Trim()
        dirty  = [bool](& git -C $RepoRoot status --porcelain 2>$null)
    }
}

# UTF-8 without BOM so Python and other tools can parse the file directly.
function Write-CnJsonFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)] $Object
    )

    $json = $Object | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText($Path, $json + "`n", (New-Object System.Text.UTF8Encoding($false)))
}
