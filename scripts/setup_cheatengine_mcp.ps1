# ============================================================================
# setup_cheatengine_mcp.ps1 — Set up Cheat Engine MCP Bridge in tools/
# ============================================================================
# Usage:
#   .\scripts\setup_cheatengine_mcp.ps1
#   .\scripts\setup_cheatengine_mcp.ps1 -CheatEngineExe "C:\Program Files\Cheat Engine 7.5\cheatengine-x86_64.exe"
# ============================================================================
param(
    [string]$CheatEngineExe = ""
)

$ErrorActionPreference = "Stop"
$Root   = Split-Path $PSScriptRoot -Parent
$McpDir = Join-Path $Root "tools\cheat-engine-mcp"
$VenvDir = Join-Path $McpDir ".venv"
$PyExe   = Join-Path $VenvDir "Scripts\python.exe"

if (-not (Test-Path (Join-Path $McpDir ".git"))) {
    Write-Host "Cloning miscusi-peek/cheatengine-mcp-bridge into tools/cheat-engine-mcp..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path (Split-Path $McpDir -Parent) | Out-Null
    git clone --depth=1 https://github.com/miscusi-peek/cheatengine-mcp-bridge.git $McpDir
}

if (-not (Test-Path $PyExe)) {
    Write-Host "Creating isolated Python venv for cheatengine-mcp-bridge..." -ForegroundColor Cyan
    python -m venv $VenvDir
}

Write-Host "Installing dependencies from MCP_Server/requirements.txt..." -ForegroundColor Cyan
& $PyExe -m pip install --upgrade pip --quiet
& $PyExe -m pip install -r (Join-Path $McpDir "MCP_Server\requirements.txt") --quiet

# Locate cheatengine-x86_64.exe
$envFile = Join-Path $Root ".env.local"
if (-not $CheatEngineExe -and (Test-Path $envFile)) {
    Get-Content $envFile | ForEach-Object {
        if ($_ -match "^CHEAT_ENGINE_EXE=(.+)$") {
            $cand = $Matches[1].Trim()
            if (Test-Path $cand) { $CheatEngineExe = $cand }
        }
    }
}

if (-not $CheatEngineExe) {
    $candidates = @()
    foreach ($base in @("C:\Program Files\Cheat Engine*", "C:\Program Files (x86)\Cheat Engine*")) {
        Get-ChildItem $base -ErrorAction SilentlyContinue | ForEach-Object {
            $candidates += (Join-Path $_.FullName "cheatengine-x86_64.exe")
            $candidates += (Join-Path $_.FullName "Cheat Engine.exe")
        }
    }
    foreach ($regPath in @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*"
    )) {
        Get-ItemProperty $regPath -ErrorAction SilentlyContinue |
            Where-Object { $_.DisplayName -match "Cheat Engine" -and $_.InstallLocation } |
            ForEach-Object {
                $candidates += (Join-Path $_.InstallLocation "cheatengine-x86_64.exe")
            }
    }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c)) { $CheatEngineExe = $c; break }
    }
}

if ($CheatEngineExe -and (Test-Path $CheatEngineExe)) {
    $ceDir      = Split-Path $CheatEngineExe -Parent
    $autorunDir = Join-Path $ceDir "autorun"
    New-Item -ItemType Directory -Force -Path $autorunDir | Out-Null
    Copy-Item (Join-Path $McpDir "MCP_Server\ce_mcp_bridge.lua") (Join-Path $autorunDir "ce_mcp_bridge.lua") -Force
    Write-Host "✓ Installed ce_mcp_bridge.lua into: $autorunDir" -ForegroundColor Green

    $ceSlash = $CheatEngineExe -replace '\\', '/'
    if (Test-Path $envFile) {
        $lines = Get-Content $envFile
        if ($lines -match "^CHEAT_ENGINE_EXE=") {
            $lines = $lines -replace "^CHEAT_ENGINE_EXE=.*$", "CHEAT_ENGINE_EXE=$ceSlash"
            [System.IO.File]::WriteAllLines($envFile, $lines, (New-Object System.Text.UTF8Encoding $false))
        } else {
            Add-Content -Path $envFile -Value "`nCHEAT_ENGINE_EXE=$ceSlash"
        }
        Write-Host "✓ Recorded CHEAT_ENGINE_EXE in .env.local" -ForegroundColor Green
    }
} else {
    Write-Warning "Cheat Engine executable not found yet. Install Cheat Engine, then re-run:"
    Write-Warning "  .\scripts\setup_cheatengine_mcp.ps1 -CheatEngineExe '<path>\cheatengine-x86_64.exe'"
}

$mcpPy = (Join-Path $McpDir "MCP_Server\mcp_cheatengine.py") -replace '\\', '/'
$pyOut = $PyExe -replace '\\', '/'
Write-Host "`n✓ cheatengine-mcp-bridge Python environment ready!" -ForegroundColor Green
Write-Host "MCP server entry (~/.gemini/config/mcp_config.json):"
Write-Host @"
{
  "mcpServers": {
    "cheatengine": {
      "command": "$pyOut",
      "args": ["$mcpPy"]
    }
  }
}
"@
