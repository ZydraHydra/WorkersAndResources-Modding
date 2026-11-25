# ============================================================================
# publish_workshop.ps1 — Build, package, and upload mod(s) per game version
# ============================================================================
# Usage:
#   .\scripts\publish_workshop.ps1 -Mod TradePriceLimits -DryRun
#   .\scripts\publish_workshop.ps1 -Mod TradePriceLimits -GameVersion 1.1.1.7
#   .\scripts\publish_workshop.ps1 -Mod TradePriceLimits -GameVersion all
# ============================================================================
param(
    [string]$Mod = "TradePriceLimits",
    [string]$Version = "",
    [string]$GameVersion = "all",
    [string]$PublishedFileId = "",
    [ValidateSet("0", "1", "2")]
    [string]$Visibility = "2",
    [string]$Changenote = "",
    [string]$SteamUser = "",
    [switch]$SkipBuild,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent

$envFile = Join-Path $Root ".env.local"
$envMap  = @{}
if (Test-Path $envFile) {
    Get-Content $envFile | ForEach-Object {
        if ($_ -match "^\s*([A-Z0-9_]+)\s*=\s*(.+)$") {
            $envMap[$Matches[1]] = $Matches[2].Trim()
        }
    }
}

if (-not $SteamUser) {
    $SteamUser = if ($env:STEAM_USERNAME) { $env:STEAM_USERNAME } elseif ($envMap.ContainsKey("STEAM_USERNAME")) { $envMap["STEAM_USERNAME"] } else { "" }
}

$steamCmd = if ($env:STEAMCMD_EXE) { $env:STEAMCMD_EXE } elseif ($envMap.ContainsKey("STEAMCMD_EXE")) { $envMap["STEAMCMD_EXE"] } else { "" }
if (-not $steamCmd) {
    $cmd = Get-Command steamcmd -ErrorAction SilentlyContinue
    if ($cmd) {
        $steamCmd = $cmd.Source
    } else {
        $candidates = @("C:\steamcmd\steamcmd.exe", "D:\steamcmd\steamcmd.exe", "C:\Tools\steamcmd\steamcmd.exe")
        $wingetPkg  = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages"
        if (Test-Path $wingetPkg) {
            $found = Get-ChildItem $wingetPkg -Recurse -Filter "steamcmd.exe" -ErrorAction SilentlyContinue |
                     Select-Object -First 1 -ExpandProperty FullName
            if ($found) { $candidates = @($found) + $candidates }
        }
        foreach ($c in $candidates) {
            if (Test-Path $c) { $steamCmd = $c; break }
        }
    }
}

$info = & (Join-Path $PSScriptRoot "get_mod_version.ps1") -Mod $Mod -VersionOverride $Version -AsObject
$ResolvedVer = $info.version

# Stage requested game-version tracks
$pkgParams = @{
    Mod             = $Mod
    Version         = $ResolvedVer
    GameVersion     = $GameVersion
    PublishedFileId = $PublishedFileId
    Visibility      = $Visibility
    Changenote      = $Changenote
    Build           = (-not $SkipBuild)
}
& (Join-Path $PSScriptRoot "package_workshop.ps1") @pkgParams
if ($LASTEXITCODE -ne 0) {
    throw "Packaging failed for $Mod"
}

$tracks = if ($GameVersion -eq "all") {
    @($info.gameVersions)
} else {
    $matched = @($info.gameVersions | Where-Object { $_.id -eq $GameVersion })
    if ($matched.Count -eq 0) { @([pscustomobject]@{ id = $GameVersion }) } else { $matched }
}

foreach ($track in $tracks) {
    $gvId            = [string]$track.id
    $publishWorkshop = if ($null -ne $track.PSObject.Properties['publishWorkshop']) { [bool]$track.publishWorkshop } else { $true }
    if ($GameVersion -eq "all" -and -not $publishWorkshop) {
        Write-Host "`nSkipping Steam Workshop upload for WRSR $gvId (publishWorkshop=false; GitHub Release ZIP staged)." -ForegroundColor DarkGray
        continue
    }
    $VdfPath = Join-Path $Root "release\${Mod}_v${ResolvedVer}_wrsr-${gvId}\workshop_item.vdf"
    if (-not (Test-Path $VdfPath)) {
        throw "Staged VDF not found at $VdfPath"
    }

    if ($DryRun) {
        Write-Host "`n[DRY RUN] ($Mod v$ResolvedVer | WRSR $gvId) VDF: $VdfPath" -ForegroundColor Yellow
        Get-Content $VdfPath | Write-Host
        $userDisplay = if ($SteamUser) { $SteamUser } else { "<STEAM_USERNAME>" }
        $cmdDisplay  = if ($steamCmd) { $steamCmd } else { "steamcmd.exe" }
        Write-Host "[DRY RUN] Command: `"$cmdDisplay`" +login $userDisplay +workshop_build_item `"$VdfPath`" +quit" -ForegroundColor Yellow
        continue
    }

    if (-not $steamCmd -or -not (Test-Path $steamCmd)) {
        throw "steamcmd.exe not found. Set STEAMCMD_EXE in .env.local or install SteamCMD."
    }
    if (-not $SteamUser) {
        throw "Steam username not set. Pass -SteamUser <name> or set STEAM_USERNAME in .env.local."
    }

    Write-Host "`n=== Uploading $Mod v$ResolvedVer [WRSR $gvId] to Steam Workshop ===" -ForegroundColor Cyan
    & $steamCmd "+login" $SteamUser "+workshop_build_item" $VdfPath "+quit"
    if ($LASTEXITCODE -ne 0) {
        throw "SteamCMD exited with code $LASTEXITCODE for WRSR $gvId"
    }

    $vdfAfter = Get-Content $VdfPath -Raw
    if ($vdfAfter -match '"publishedfileid"\s+"(\d+)"') {
        $newId = $Matches[1]
        if ($newId -ne "0") {
            Write-Host "✓ Workshop upload succeeded [$gvId]! PublishedFileId = $newId" -ForegroundColor Green
            $gvPath = Join-Path $Root "mods\$Mod\game_versions.json"
            if (Test-Path $gvPath) {
                $gvObj = Get-Content $gvPath -Raw | ConvertFrom-Json
                $changed = $false
                foreach ($item in $gvObj.gameVersions) {
                    if ($item.id -eq $gvId -and $item.publishedFileId -ne $newId) {
                        $item.publishedFileId = $newId
                        $changed = $true
                    }
                }
                if ($changed) {
                    $jsonOut = $gvObj | ConvertTo-Json -Depth 10
                    [System.IO.File]::WriteAllText($gvPath, $jsonOut + "`n", (New-Object System.Text.UTF8Encoding $false))
                    Write-Host "  Updated $gvPath with PublishedFileId=$newId for WRSR $gvId." -ForegroundColor Cyan
                }
            }
        }
    }
}
