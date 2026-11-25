# ============================================================================
# package_workshop.ps1 — Stage release folders per mod & WRSR game version
# ============================================================================
# Usage:
#   .\scripts\package_workshop.ps1 -Mod TradePriceLimits
#   .\scripts\package_workshop.ps1 -Mod TradePriceLimits -GameVersion 1.1.1.7 -Build
#   .\scripts\package_workshop.ps1 -Mod TradePriceLimits -GameVersion all -Build
# ============================================================================
param(
    [string]$Mod = "TradePriceLimits",
    [string]$Version = "",
    [string]$GameVersion = "all",
    [string]$PublishedFileId = "",
    [ValidateSet("0", "1", "2")]
    [string]$Visibility = "2",
    [string]$Changenote = "",
    [switch]$Build
)

$ErrorActionPreference = "Stop"

function New-FallbackPreviewPng {
    param(
        [string]$OutPath,
        [string]$Title,
        [string]$Subtitle
    )
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap 512, 512
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $gfx.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

    $bgBrush     = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 31, 31, 26))
    $panelBrush  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 42, 40, 32))
    $goldPen     = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 214, 166, 69)), 6
    $goldBrush   = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 214, 166, 69))
    $creamBrush  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 232, 224, 204))
    $mutedBrush  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 168, 160, 138))

    $gfx.FillRectangle($bgBrush, 0, 0, 512, 512)
    $gfx.FillRectangle($panelBrush, 24, 24, 464, 464)
    $gfx.DrawRectangle($goldPen, 24, 24, 464, 464)
    $gfx.FillRectangle($goldBrush, 24, 24, 464, 14)

    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = [System.Drawing.StringAlignment]::Center
    $sf.LineAlignment = [System.Drawing.StringAlignment]::Center

    $tagFont   = New-Object System.Drawing.Font "Segoe UI", 16, ([System.Drawing.FontStyle]::Bold)
    $titleFont = New-Object System.Drawing.Font "Segoe UI", 28, ([System.Drawing.FontStyle]::Bold)
    $subFont   = New-Object System.Drawing.Font "Segoe UI", 16, ([System.Drawing.FontStyle]::Regular)

    $gfx.DrawString("WRSR NATIVE PLUGIN", $tagFont, $goldBrush, (New-Object System.Drawing.RectangleF 36, 72, 440, 40), $sf)
    $gfx.DrawString($Title, $titleFont, $creamBrush, (New-Object System.Drawing.RectangleF 36, 170, 440, 120), $sf)
    $gfx.DrawString($Subtitle, $subFont, $mutedBrush, (New-Object System.Drawing.RectangleF 36, 330, 440, 80), $sf)
    $gfx.DrawString("RML  |  TESMIOLOADER  |  X64", $tagFont, $goldBrush, (New-Object System.Drawing.RectangleF 36, 428, 440, 40), $sf)

    $bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $gfx.Dispose()
    $bmp.Dispose()
}

function Convert-MarkdownToSteamBBCode {
    param([string]$Text)
    $t = $Text -replace '\*\*([^*]+)\*\*', '[b]$1[/b]'
    $t = $t -replace '`([^`]+)`', '[i]$1[/i]'
    $t = $t -replace '\[([^\]]+)\]\((https?://[^)]+)\)', '[url=$2]$1[/url]'
    return $t
}

function Get-ModChangelogData {
    param(
        [string]$RootDir,
        [string]$ModName,
        [string]$ResolvedVer
    )
    $modDir       = Join-Path $RootDir "mods\$ModName"
    $changelogMd  = Join-Path $modDir "CHANGELOG.md"
    $latestPlain  = @()
    $bbSections   = @()

    if (Test-Path $changelogMd) {
        $lines = Get-Content $changelogMd -Encoding UTF8
        $sections = @()
        $curTitle = $null
        $curItems = @()
        foreach ($raw in $lines) {
            $line = $raw.Trim()
            if ($line -match '^##\s+(.+)$') {
                if ($curTitle) {
                    $sections += [pscustomobject]@{ title = $curTitle; items = $curItems }
                }
                $curTitle = ($Matches[1] -replace '^\[|\]$', '').Trim()
                $curItems = @()
            } elseif ($curTitle -and $line -match '^[-*]\s+(.+)$') {
                $curItems += $Matches[1].Trim()
            }
        }
        if ($curTitle) {
            $sections += [pscustomobject]@{ title = $curTitle; items = $curItems }
        }

        if ($sections.Count -gt 0) {
            $top = $sections[0]
            foreach ($item in $top.items) {
                $plain = $item -replace '\*\*([^*]+)\*\*', '$1' -replace '`([^`]+)`', '$1'
                $latestPlain += "- $plain"
            }

            $maxSections = [Math]::Min(3, $sections.Count)
            for ($i = 0; $i -lt $maxSections; $i++) {
                $sec = $sections[$i]
                $bb  = "[b]v$($sec.title)[/b][list]"
                foreach ($item in $sec.items) {
                    $bb += "`n[*] $(Convert-MarkdownToSteamBBCode $item)"
                }
                $bb += "`n[/list]"
                $bbSections += $bb
            }
        }
    }

    if ($latestPlain.Count -eq 0) {
        $git = Get-Command git -ErrorAction SilentlyContinue
        if ($git -and (Test-Path (Join-Path $RootDir ".git"))) {
            Push-Location $RootDir
            $prevEap = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            try {
                $lastTag = (& git describe --tags --match "$ModName-*v*" --abbrev=0 2>$null | Select-Object -First 1)
                $range   = if ($LASTEXITCODE -eq 0 -and $lastTag) { "$lastTag..HEAD" } else { "-n 8" }
                $commits = & git log $range --no-merges --pretty=format:"- %s" -- "mods/$ModName" "common" 2>$null
                if ($LASTEXITCODE -eq 0 -and $commits) {
                    $latestPlain = @($commits)
                }
            } finally {
                $global:LASTEXITCODE = 0
                $ErrorActionPreference = $prevEap
                Pop-Location
            }
        }
    }

    $plainBlock = if ($latestPlain.Count -gt 0) {
        "Release v$ResolvedVer`n" + ($latestPlain -join "`n")
    } else {
        "Release v$ResolvedVer"
    }

    $bbBlock = if ($bbSections.Count -gt 0) {
        "`n`n[h1]Recent Changelog[/h1]" + ($bbSections -join "`n")
    } else {
        ""
    }

    return [pscustomobject]@{
        changenote = $plainBlock
        bbSection  = $bbBlock
        bbSections = $bbSections
    }
}

$Root   = Split-Path $PSScriptRoot -Parent
$ModDir = Join-Path $Root "mods\$Mod"

$info = & (Join-Path $PSScriptRoot "get_mod_version.ps1") -Mod $Mod -VersionOverride $Version -AsObject
$ResolvedVer = $info.version

$targetTracks = if ($GameVersion -eq "all") {
    @($info.gameVersions)
} else {
    $matched = @($info.gameVersions | Where-Object { $_.id -eq $GameVersion })
    if ($matched.Count -eq 0) {
        @([pscustomobject]@{
            id                    = $GameVersion
            label                 = "WRSR $GameVersion"
            supportedGameVersions = if ($GameVersion -eq "latest") { @() } else { @($GameVersion) }
            publishedFileId       = "0"
        })
    } else {
        $matched
    }
}

foreach ($track in $targetTracks) {
    $gvId       = [string]$track.id
    $gvLabel    = if ($track.label) { [string]$track.label } else { "WRSR $gvId" }
    $BuildDir   = Join-Path $Root "build\release\$gvId\mods\$Mod"
    $StageDir   = Join-Path $Root "release\${Mod}_v${ResolvedVer}_wrsr-${gvId}"
    $ContentDir = Join-Path $StageDir "content"
    $PluginDir  = Join-Path $ContentDir "plugins"

    if ($Build) {
        & (Join-Path $PSScriptRoot "build.ps1") -Mod $Mod -Config Release -GameVersion $gvId -VersionOverride $ResolvedVer
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed for $Mod ($gvId)"
        }
    }

    $DllSrc = Join-Path $BuildDir "$Mod.dll"
    if (-not (Test-Path $DllSrc)) {
        throw "Release DLL not found at $DllSrc — run .\scripts\build.ps1 -Mod $Mod -GameVersion $gvId (or pass -Build)"
    }
    if ((Get-Item $DllSrc).Length -le 0) {
        throw "Release DLL is empty at $DllSrc"
    }

    if (Test-Path $StageDir) {
        Remove-Item $StageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $PluginDir | Out-Null

    # 1. Copy compiled DLL
    Copy-Item $DllSrc $PluginDir -Force

    # 2. Copy & validate/stamp RML manifest (prefer stamped CMake manifest if present)
    $ManBuilt = Join-Path $BuildDir "$Mod.rml.json"
    $ManSrc   = if (Test-Path $ManBuilt) { $ManBuilt } else { Join-Path $ModDir "$Mod.rml.json" }
    if (Test-Path $ManSrc) {
        $manifestObj = Get-Content $ManSrc -Raw -Encoding UTF8 | ConvertFrom-Json
        $manifestObj.version = $ResolvedVer
        if ($manifestObj.description) {
            $descBase = ([string]$manifestObj.description) -replace '\s*\(v[0-9]+\.[0-9]+\.[0-9]+\)$', ''
            $manifestObj.description = "$descBase (v$ResolvedVer)"
        }
        if ($null -ne $track.supportedGameVersions) {
            $manifestObj.supportedGameVersions = @($track.supportedGameVersions)
        }
        $manifestJson = $manifestObj | ConvertTo-Json -Depth 10
        [System.IO.File]::WriteAllText((Join-Path $PluginDir "$Mod.rml.json"), $manifestJson + "`n", (New-Object System.Text.UTF8Encoding $false))
    }

    # 2b. Copy default global plugin INI beside the DLL (if present) so mod managers expose it
    $IniSrc = Join-Path $ModDir "$Mod.ini"
    if (Test-Path $IniSrc) {
        Copy-Item $IniSrc $PluginDir -Force
    }

    # 3. Resolve PublishedFileId (param -> env WORKSHOP_ID_<MOD>_<GV> -> game_versions.json -> 0)
    $envKeygv = ("WORKSHOP_ID_{0}_{1}" -f ($Mod.ToUpper() -replace '[^A-Z0-9]', '_'), ($gvId.ToUpper() -replace '[^A-Z0-9]', '_'))
    $resolvedId = $PublishedFileId.Trim()
    if (-not $resolvedId -and [Environment]::GetEnvironmentVariable($envKeygv)) {
        $resolvedId = [Environment]::GetEnvironmentVariable($envKeygv).Trim()
    }
    if (-not $resolvedId -and $env:WORKSHOP_PUBLISHED_FILE_ID) {
        $resolvedId = $env:WORKSHOP_PUBLISHED_FILE_ID.Trim()
    }
    if (-not $resolvedId -and $track.publishedFileId) {
        $resolvedId = ([string]$track.publishedFileId).Trim()
    }
    if (-not $resolvedId) {
        $resolvedId = "0"
    }

    # 4. Copy or generate previewimage.png (< 1 MB required by Steam)
    $PreviewStage   = Join-Path $StageDir "previewimage.png"
    $PreviewContent = Join-Path $ContentDir "previewimage.png"
    $PreviewSrc     = Join-Path $ModDir "steamcmd\previewimage.png"
    if (Test-Path $PreviewSrc) {
        Copy-Item $PreviewSrc $PreviewStage -Force
    } else {
        New-FallbackPreviewPng -OutPath $PreviewStage -Title $Mod -Subtitle "v$ResolvedVer | $gvLabel"
    }
    Copy-Item $PreviewStage $PreviewContent -Force

    $previewSize = (Get-Item $PreviewStage).Length
    if ($previewSize -gt 1048576) {
        throw "previewimage.png exceeds Steam's 1 MB limit ($previewSize bytes)"
    }

    # 5. Copy workshopconfig.ini into content/
    $CfgSrc = Join-Path $ModDir "steamcmd\workshopconfig.ini"
    if (Test-Path $CfgSrc) {
        $cfgText = Get-Content $CfgSrc -Raw -Encoding UTF8
        if ($resolvedId -and $resolvedId -ne "0") {
            $cfgText = $cfgText -replace '(?m)^\$ITEM_ID\s+\S+', "`$ITEM_ID $resolvedId"
        }
        $cfgText = $cfgText -replace '(?m)^\$VISIBILITY\s+\S+', "`$VISIBILITY $Visibility"
        $cfgText = $cfgText -replace '(?m)^\$ITEM_NAME\s+".*"', "`$ITEM_NAME `"$Mod [$gvLabel]`""
        $cfgText = $cfgText -replace '\{VERSION\}', $ResolvedVer
        [System.IO.File]::WriteAllText((Join-Path $ContentDir "workshopconfig.ini"), $cfgText, (New-Object System.Text.UTF8Encoding $false))
    }

    # 6. Render clean workshop_item.vdf OUTSIDE content/
    $isPrimaryTrack  = ($null -ne $track.isPrimary -and [bool]$track.isPrimary) -or ($gvId -eq "latest")
    $publishWorkshop = if ($null -ne $track.PSObject.Properties['publishWorkshop']) { [bool]$track.publishWorkshop } else { $true }
    $workshopTitle   = if ($isPrimaryTrack) { "$Mod" } else { "$Mod [$gvLabel]" }

    $primaryTrack = $info.gameVersions | Where-Object { ($null -ne $_.isPrimary -and [bool]$_.isPrimary) -or $_.id -eq "latest" } | Select-Object -First 1
    $primaryId    = if ($primaryTrack -and $primaryTrack.publishedFileId) { [string]$primaryTrack.publishedFileId } else { "" }

    $otherLinks = @()
    foreach ($other in $info.gameVersions) {
        $otherPublish = if ($null -ne $other.PSObject.Properties['publishWorkshop']) { [bool]$other.publishWorkshop } else { $true }
        if ($other.id -ne $gvId -and $otherPublish -and $other.publishedFileId -and $other.publishedFileId -ne "0") {
            $otherLabel = if ($other.label) { [string]$other.label } else { "WRSR $($other.id)" }
            $otherLinks += "[url=https://steamcommunity.com/sharedfiles/filedetails/?id=$($other.publishedFileId)]$otherLabel[/url]"
        }
    }

    if ($isPrimaryTrack) {
        $supportedList = @($track.supportedGameVersions) -join ", "
        $verSuffix     = if ($supportedList) { " Supports WRSR $supportedList." } else { "" }
        $editionHeader = "[b]Main Unified Edition (Mod v$ResolvedVer - Alpha):[/b] Automatically detects your WRSR build and loads matching offsets under this single Workshop ID.$verSuffix"
        if ($otherLinks.Count -gt 0) {
            $editionHeader += "`nOther tracks: " + ($otherLinks -join " | ")
        }
    } else {
        $editionHeader = "[b]Legacy Pinned Edition:[/b] Built for [b]$gvLabel[/b] (Mod v$ResolvedVer - Alpha)."
        if ($primaryId -and $primaryId -ne "0") {
            $editionHeader += "`nLooking for the unified auto-updating Workshop item? [url=https://steamcommunity.com/sharedfiles/filedetails/?id=$primaryId]$Mod (Unified)[/url]"
        }
    }

    $changelogData = Get-ModChangelogData -RootDir $Root -ModName $Mod -ResolvedVer $ResolvedVer
    $note = if ($Changenote) { $Changenote } else { $changelogData.changenote }
    $VdfSrc = Join-Path $ModDir "steamcmd\workshop_item.vdf"
    $emitVdf = $publishWorkshop
    if ($emitVdf -and (Test-Path $VdfSrc)) {
        $contentSlash = $ContentDir -replace '\\', '/'
        $previewSlash = $PreviewStage -replace '\\', '/'
        $vdf = Get-Content $VdfSrc -Raw -Encoding UTF8
        $vdf = $vdf -replace '\{WORKSHOP_TITLE\}', $workshopTitle
        $vdf = $vdf -replace '\{EDITION_HEADER\}', $editionHeader
        $vdf = $vdf -replace '\{VERSION\}', $ResolvedVer
        $vdf = $vdf -replace '\{GAME_VERSION\}', $gvId
        $vdf = $vdf -replace '\{GAME_LABEL\}', $gvLabel
        $vdf = $vdf -replace '\{PUBLISHED_FILE_ID\}', $resolvedId
        $vdf = $vdf -replace '\{CONTENT_DIR\}', $contentSlash
        $vdf = $vdf -replace '\{PREVIEW_FILE\}', $previewSlash
        $vdf = $vdf -replace '\{VISIBILITY\}', $Visibility
        $vdf = $vdf -replace '\{CHANGENOTE\}', ($note -replace '"', "'")

        # Steam Workshop caps ISteamUGC::SetItemDescription at 8,000 characters.
        # Fit as many recent changelog sections cleanly within 7,900 chars.
        $maxDescChars = 7900
        $baseDescLen = 0
        if ($vdf -match '(?s)"description"\s+"(.*?)"\s+"language"') {
            $baseDescLen = ($Matches[1] -replace '\{CHANGELOG_SECTION\}', '').Length
        }
        $fittedChangelog = ""
        $sectionsList = @($changelogData.bbSections)
        if ($sectionsList.Count -gt 0 -and $baseDescLen -gt 0) {
            $header = "`n`n[h1]Recent Changelog[/h1]"
            $candidate = $header
            $added = 0
            foreach ($secBb in $sectionsList) {
                $cleanSec = $secBb -replace '"', "'"
                $next = if ($added -eq 0) { $candidate + $cleanSec } else { $candidate + "`n" + $cleanSec }
                if (($baseDescLen + $next.Length) -le $maxDescChars) {
                    $candidate = $next
                    $added++
                } else {
                    break
                }
            }
            if ($added -gt 0) {
                $fittedChangelog = $candidate
            }
        }
        $vdf = $vdf -replace '\{CHANGELOG_SECTION\}', $fittedChangelog

        if ($vdf -match '(?s)"description"\s+"(.*?)"\s+"language"') {
            $finalLen = $Matches[1].Length
            if ($finalLen -gt 8000) {
                throw "Steam Workshop description exceeds 8,000-char limit ($finalLen chars)"
            }
            Write-Host "  Description : $finalLen / 8000 chars"
        }

        [System.IO.File]::WriteAllText((Join-Path $StageDir "workshop_item.vdf"), $vdf, (New-Object System.Text.UTF8Encoding $false))
    }

    # 7. Create standalone ZIP for GitHub Releases / manual install
    $ZipPath = Join-Path $StageDir "${Mod}_v${ResolvedVer}_wrsr-${gvId}.zip"
    Compress-Archive -Path "$ContentDir\*" -DestinationPath $ZipPath -Force

    Write-Host "✓ Staged $Mod v$ResolvedVer [$gvLabel] -> $StageDir" -ForegroundColor Green
    Write-Host "  Workshop ID : $resolvedId (Steam upload: $(if ($emitVdf) { 'enabled' } else { 'skipped / GitHub ZIP only' }))"
    Write-Host "  Visibility  : $Visibility (0=Public, 1=Friends, 2=Hidden)"
    Write-Host "  Archive zip : $ZipPath"
}
