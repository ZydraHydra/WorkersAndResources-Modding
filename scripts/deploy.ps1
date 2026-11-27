# ============================================================================
# deploy.ps1 — Deploy built mod DLL(s) + manifests to GRML / RML plugins dir
# ============================================================================
# Usage:
#   .\scripts\deploy.ps1 [-Mod TradePriceLimits | -Mod all] [-Config Release] [-GameVersion 1.2.0.9] [-IncludeWorkshop]
# ============================================================================
param(
    [Alias("Target")]
    [string]$Mod = "TradePriceLimits",
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string]$GameVersion = "",
    [switch]$IncludeWorkshop
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent

# ── Verify third_party/grml/grml_api.h parity with sibling GRML repo ─────────
$mirrorHeader    = Join-Path $Root "third_party\grml\grml_api.h"
$canonicalHeader = Join-Path (Split-Path $Root -Parent) "GloriousRepublicModLoader\include\grml_api.h"
if (Test-Path $canonicalHeader) {
    if (-not (Test-Path $mirrorHeader)) {
        throw "Mirrored header missing at $mirrorHeader"
    }
    $hCanon  = (Get-FileHash -Algorithm SHA256 $canonicalHeader).Hash
    $hMirror = (Get-FileHash -Algorithm SHA256 $mirrorHeader).Hash
    if ($hCanon -ne $hMirror) {
        throw "grml_api.h mismatch between $canonicalHeader ($hCanon) and $mirrorHeader ($hMirror). Sync headers before deploying!"
    }
    Write-Host "✓ Verified grml_api.h SHA-256 parity ($($hCanon.Substring(0, 12))...)" -ForegroundColor DarkGreen
}

# ── Resolve target plugin directories (GRML and/or RML) ──────────────────────
$envFile        = Join-Path $Root ".env.local"
$grmlPluginsDir = $null
$grmlDir        = $null
$rmlPluginsDir  = $null
$rmlDir         = $null
$workshopDir    = $null
$envGameVer     = $null

if (Test-Path $envFile) {
    Get-Content $envFile | ForEach-Object {
        if ($_ -match "^GRML_PLUGINS_DIR=(.+)$") {
            $grmlPluginsDir = $Matches[1].Trim() -replace '/', '\'
        } elseif ($_ -match "^GRML_DIR=(.+)$") {
            $grmlDir = $Matches[1].Trim() -replace '/', '\'
        } elseif ($_ -match "^RML_PLUGINS_DIR=(.+)$") {
            $rmlPluginsDir = $Matches[1].Trim() -replace '/', '\'
        } elseif ($_ -match "^RML_DIR=(.+)$") {
            $rmlDir = $Matches[1].Trim() -replace '/', '\'
        } elseif ($_ -match "^WORKSHOP_DIR=(.+)$") {
            $workshopDir = $Matches[1].Trim() -replace '/', '\'
        } elseif ($_ -match "^WRSR_GAME_VERSION=(.+)$") {
            $envGameVer = $Matches[1].Trim()
        }
    }
}

if (-not $grmlPluginsDir -and $grmlDir) {
    $grmlPluginsDir = Join-Path $grmlDir "plugins"
}
if (-not $grmlPluginsDir) {
    $siblingGrmlBin = Join-Path (Split-Path $Root -Parent) "GloriousRepublicModLoader\build\$($Config.ToLower())\bin"
    if (Test-Path $siblingGrmlBin) {
        $grmlPluginsDir = Join-Path $siblingGrmlBin "plugins"
    }
}

if ($IncludeWorkshop) {
    if (-not $rmlPluginsDir -and $rmlDir) {
        foreach ($sub in @("rml\plugins", "rml\plugin", "plugins", "plugin")) {
            $cand = Join-Path $rmlDir $sub
            if (Test-Path $cand) { $rmlPluginsDir = $cand; break }
        }
        if (-not $rmlPluginsDir) { $rmlPluginsDir = Join-Path $rmlDir "rml\plugins" }
    }
    if (-not $rmlPluginsDir -and $workshopDir) {
        $candRml = Join-Path $workshopDir "3787969749\rml\plugins"
        if (Test-Path (Join-Path $workshopDir "3787969749")) { $rmlPluginsDir = $candRml }
    }
}

$targetDirs = @()
if ($grmlPluginsDir -and ($IncludeWorkshop -or ($grmlPluginsDir -notmatch "steamapps\\workshop\\content"))) {
    $targetDirs += $grmlPluginsDir
}
if ($IncludeWorkshop -and $rmlPluginsDir -and ($rmlPluginsDir -ne $grmlPluginsDir)) {
    $targetDirs += $rmlPluginsDir
}

if ($targetDirs.Count -eq 0) {
    throw "No local GRML deploy target configured (Steam Workshop targets are skipped unless -IncludeWorkshop is explicitly specified)."
}

[string[]]$modNames = if ($Mod -eq "all") {
    @(Get-ChildItem (Join-Path $Root "mods") -Directory | Select-Object -ExpandProperty Name)
} else {
    @($Mod)
}

foreach ($m in $modNames) {
    $gv = $GameVersion
    if (-not $gv) {
        if ($envGameVer) {
            $gv = $envGameVer
        } else {
            $info = & (Join-Path $PSScriptRoot "get_mod_version.ps1") -Mod $m -AsObject
            $gv = $info.defaultGameVersion
        }
    }

    $BuildDir = Join-Path $Root "build\$($Config.ToLower())\$gv\mods\$m"
    $DllSrc   = Join-Path $BuildDir "$m.dll"
    $ManSrc   = Join-Path $BuildDir "$m.rml.json"
    if (-not (Test-Path $ManSrc)) {
        $ManSrc = Join-Path $Root "mods\$m\$m.rml.json"
    }
    $IniSrc = Join-Path $BuildDir "$m.ini"
    if (-not (Test-Path $IniSrc)) {
        $IniSrc = Join-Path $Root "mods\$m\$m.ini"
    }

    if (-not (Test-Path $DllSrc)) {
        throw "Built DLL not found at $DllSrc — run .\scripts\build.ps1 -Mod $m -GameVersion $gv first"
    }

    foreach ($TargetDir in $targetDirs) {
        New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null
        Copy-Item $DllSrc $TargetDir -Force
        if (Test-Path $ManSrc) {
            Copy-Item $ManSrc $TargetDir -Force
        }
        $GrmlManSrc = Join-Path $BuildDir "$m.grml.json"
        if (-not (Test-Path $GrmlManSrc)) {
            $GrmlManSrc = Join-Path $Root "mods\$m\$m.grml.json"
        }
        if (Test-Path $GrmlManSrc) {
            Copy-Item $GrmlManSrc $TargetDir -Force
        }
        $IniDest   = Join-Path $TargetDir "$m.ini"
        $iniCopied = $false
        if ((Test-Path $IniSrc) -and -not (Test-Path $IniDest)) {
            Copy-Item $IniSrc $IniDest
            $iniCopied = $true
        }
        Write-Host "✓ Deployed $m (WRSR $gv) -> $TargetDir" -ForegroundColor Green
        if ($iniCopied) { Write-Host "  $m.ini (created)" }
    }
}
