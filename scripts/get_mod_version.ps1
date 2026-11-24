# ============================================================================
# get_mod_version.ps1 — Resolve automatic per-mod SemVer & game target tracks
# ============================================================================
# Usage:
#   .\scripts\get_mod_version.ps1 -Mod TradePriceLimits [-VersionOverride ""]
# Returns JSON (or object with -AsObject) containing Version and GameVersions.
# ============================================================================
param(
    [Parameter(Mandatory = $true)]
    [string]$Mod,
    [string]$VersionOverride = "",
    [switch]$AsObject
)

$ErrorActionPreference = "Stop"
$Root   = Split-Path $PSScriptRoot -Parent
$ModDir = Join-Path $Root "mods\$Mod"

if (-not (Test-Path $ModDir)) {
    throw "Mod directory not found: $ModDir"
}

$baseVer = "1.0"
$verFile = Join-Path $ModDir "VERSION"
if (Test-Path $verFile) {
    $baseVer = (Get-Content $verFile -Raw).Trim()
}

$major = 1
$minor = 0
$patch = 0
$hasExplicitPatch = $false
if ($baseVer -match '^(\d+)\.(\d+)\.(\d+)$') {
    $major = [int]$Matches[1]
    $minor = [int]$Matches[2]
    $patch = [int]$Matches[3]
    $hasExplicitPatch = $true
} elseif ($baseVer -match '^(\d+)\.(\d+)$') {
    $major = [int]$Matches[1]
    $minor = [int]$Matches[2]
}

$resolved = "$major.$minor.$patch"
if ($VersionOverride) {
    $resolved = $VersionOverride.Trim()
} elseif (-not $hasExplicitPatch) {
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git -and (Test-Path (Join-Path $Root ".git"))) {
        Push-Location $Root
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $headRef = (& git rev-parse --verify HEAD 2>$null)
            if ($LASTEXITCODE -eq 0 -and $headRef) {
                $tagMatch = (& git tag --points-at HEAD "$Mod-*v*" 2>$null) | Select-Object -First 1
                if ($tagMatch -and ($tagMatch -match '-v(\d+\.\d+\.\d+.*)$')) {
                    $resolved = $Matches[1]
                } else {
                    $countRaw = (& git rev-list --count HEAD -- "mods/$Mod" "common" 2>$null)
                    if ($LASTEXITCODE -eq 0 -and ($countRaw -match '^\d+$')) {
                        $patch    = [int]$countRaw
                        $resolved = "$major.$minor.$patch"
                    }
                }
            }
        } finally {
            $global:LASTEXITCODE = 0
            $ErrorActionPreference = $prevEap
            Pop-Location
        }
    }
}

$gvPath = Join-Path $ModDir "game_versions.json"
$defaultGv = "latest"
$gameVersions = @(
    [pscustomobject]@{
        id                    = "latest"
        label                 = "WRSR Latest"
        supportedGameVersions = @()
        publishedFileId       = "0"
    }
)
if (Test-Path $gvPath) {
    $gvObj = Get-Content $gvPath -Raw | ConvertFrom-Json
    if ($gvObj.defaultGameVersion) {
        $defaultGv = [string]$gvObj.defaultGameVersion
    }
    if ($gvObj.gameVersions -and $gvObj.gameVersions.Count -gt 0) {
        $gameVersions = @($gvObj.gameVersions)
    }
}

$result = [pscustomobject]@{
    mod                = $Mod
    version            = $resolved
    defaultGameVersion = $defaultGv
    gameVersions       = $gameVersions
}

if ($AsObject) {
    return $result
}
$result | ConvertTo-Json -Depth 6
