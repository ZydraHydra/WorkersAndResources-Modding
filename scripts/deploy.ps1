# ============================================================================
# deploy.ps1 — Deploy a built mod DLL + .rml.json into local RML plugins dir
# ============================================================================
# Usage: .\scripts\deploy.ps1 [-Mod TradePriceLimits] [-Config Release] [-GameVersion 1.1.1.7]
# ============================================================================
param(
    [string]$Mod = "TradePriceLimits",
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string]$GameVersion = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent

$envFile       = Join-Path $Root ".env.local"
$rmlPluginsDir = $null
$envGameVer    = $null
if (Test-Path $envFile) {
    Get-Content $envFile | ForEach-Object {
        if ($_ -match "^RML_PLUGINS_DIR=(.+)$") {
            $rmlPluginsDir = $Matches[1].Trim() -replace '/', '\'
        } elseif ($_ -match "^WRSR_GAME_VERSION=(.+)$") {
            $envGameVer = $Matches[1].Trim()
        }
    }
}

if (-not $GameVersion) {
    if ($envGameVer) {
        $GameVersion = $envGameVer
    } else {
        $info = & (Join-Path $PSScriptRoot "get_mod_version.ps1") -Mod $Mod -AsObject
        $GameVersion = $info.defaultGameVersion
    }
}

$BuildDir = Join-Path $Root "build\$($Config.ToLower())\$GameVersion\mods\$Mod"
$DllSrc   = Join-Path $BuildDir "$Mod.dll"
$ManSrc   = Join-Path $BuildDir "$Mod.rml.json"
if (-not (Test-Path $ManSrc)) {
    $ManSrc = Join-Path $Root "mods\$Mod\$Mod.rml.json"
}
$IniSrc   = Join-Path $BuildDir "$Mod.ini"
if (-not (Test-Path $IniSrc)) {
    $IniSrc = Join-Path $Root "mods\$Mod\$Mod.ini"
}

if (-not (Test-Path $DllSrc)) {
    throw "Built DLL not found at $DllSrc — run .\scripts\build.ps1 -Mod $Mod -GameVersion $GameVersion first"
}

if (-not $rmlPluginsDir) {
    throw "RML_PLUGINS_DIR not configured in .env.local"
}

$TargetDir = $rmlPluginsDir
New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null

Copy-Item $DllSrc $TargetDir -Force
if (Test-Path $ManSrc) {
    Copy-Item $ManSrc $TargetDir -Force
}
$IniDest = Join-Path $TargetDir "$Mod.ini"
$iniCopied = $false
if ((Test-Path $IniSrc) -and -not (Test-Path $IniDest)) {
    Copy-Item $IniSrc $IniDest
    $iniCopied = $true
}

Write-Host "✓ Deployed $Mod (WRSR $GameVersion) to: $TargetDir" -ForegroundColor Green
Write-Host "  $Mod.dll"
if (Test-Path $ManSrc) { Write-Host "  $Mod.rml.json" }
if ($iniCopied) { Write-Host "  $Mod.ini (created)" } elseif (Test-Path $IniDest) { Write-Host "  $Mod.ini (preserved)" }
Write-Host "`nLaunch WRSR via RepublicModLoader.exe to test."
