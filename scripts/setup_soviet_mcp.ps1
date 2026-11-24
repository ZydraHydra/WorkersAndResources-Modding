# ============================================================================
# setup_soviet_mcp.ps1 — Install burnshall-ui/SovietRepublic-MCP into references/
# ============================================================================
# Usage: .\scripts\setup_soviet_mcp.ps1
# ============================================================================

$ErrorActionPreference = "Stop"
$Root    = Split-Path $PSScriptRoot -Parent
$McpDir  = Join-Path $Root "references\SovietRepublic-MCP"
$VenvDir = Join-Path $McpDir ".venv"
$PyExe   = Join-Path $VenvDir "Scripts\python.exe"

if (-not (Test-Path $McpDir)) {
    Write-Host "Cloning burnshall-ui/SovietRepublic-MCP into references/..." -ForegroundColor Cyan
    git clone --depth=1 https://github.com/burnshall-ui/SovietRepublic-MCP.git $McpDir
}

if (-not (Test-Path $PyExe)) {
    Write-Host "Creating isolated Python venv for SovietRepublic-MCP..." -ForegroundColor Cyan
    python -m venv $VenvDir
}

Write-Host "Installing dependencies from requirements.txt..." -ForegroundColor Cyan
& $PyExe -m pip install --upgrade pip --quiet
& $PyExe -m pip install -r (Join-Path $McpDir "requirements.txt") "pydantic>=2.7,<2.11" --quiet

$Runner = Join-Path $Root "scripts\run_soviet_mcp.py"

Write-Host "`n✓ SovietRepublic-MCP ready!" -ForegroundColor Green
Write-Host "Add this entry to your MCP config (~/.gemini/config/mcp_config.json):"
Write-Host @"
{
  "mcpServers": {
    "soviet-republic": {
      "command": "$($PyExe -replace '\\', '/')",
      "args": ["$($Runner -replace '\\', '/')"]
    }
  }
}
"@
