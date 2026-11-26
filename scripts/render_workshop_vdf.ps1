# ============================================================================
# render_workshop_vdf.ps1 — Render workshop_item.vdf from base template
# ============================================================================
# Usage: .\scripts\render_workshop_vdf.ps1 -Mod CODispatchControl
# ============================================================================
param(
    [Parameter(Mandatory = $true)]
    [string]$Mod
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$TplPath = Join-Path $Root "templates\workshop_description.vdf.tpl"
$SectionsPath = Join-Path $Root "mods\$Mod\steamcmd\workshop_sections.json"
$OutPath = Join-Path $Root "mods\$Mod\steamcmd\workshop_item.vdf"

if (-not (Test-Path $TplPath)) { throw "Base template not found: $TplPath" }
if (-not (Test-Path $SectionsPath)) { throw "Mod workshop sections not found: $SectionsPath" }

$sec = Get-Content $SectionsPath -Raw -Encoding UTF8 | ConvertFrom-Json
$tpl = Get-Content $TplPath -Raw -Encoding UTF8

$tpl = $tpl -replace '\{MOD_NAME\}', $Mod
$tpl = $tpl -replace '\{HOTKEY\}', [string]$sec.hotkey
$tpl = $tpl -replace '\{INTRO_TITLE\}', [string]$sec.introTitle
$tpl = $tpl -replace '\{INTRO_BODY\}', [string]$sec.introBody
$tpl = $tpl -replace '\{OPERATING_SECTION\}', [string]$sec.operatingSection
$tpl = $tpl -replace '\{EXAMPLES_SECTION\}', [string]$sec.examplesSection
$tpl = $tpl -replace '\{GUARANTEES_SECTION\}', [string]$sec.guaranteesSection
$tpl = $tpl -replace '\{COMPANION_MODS_SECTION\}', [string]$sec.companionModsSection

[System.IO.File]::WriteAllText($OutPath, $tpl, (New-Object System.Text.UTF8Encoding $false))
Write-Host "✓ Rendered workshop_item.vdf -> $OutPath" -ForegroundColor Green
