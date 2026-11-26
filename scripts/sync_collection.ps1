# ============================================================================
# sync_collection.ps1 — Sync workshop_collection.json & README.md from mods/
#                        and optionally verify against live Steam Collection
# ============================================================================
# Usage:
#   .\scripts\sync_collection.ps1
#   .\scripts\sync_collection.ps1 -CheckOnly
#   .\scripts\sync_collection.ps1 -CheckOnly -CheckSteam
#   .\scripts\sync_collection.ps1 -CheckSteam -Strict
# ============================================================================
param(
    [switch]$CheckOnly,
    [switch]$CheckSteam,
    [switch]$Strict,
    [string]$SummaryFile = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$CollectionPath = Join-Path $Root "workshop_collection.json"
$DescPath = Join-Path $Root "workshop_collection_description.txt"
$PreviewPath = Join-Path $Root "workshop_collection_preview.png"
$BackgroundPath = Join-Path $Root "workshop_collection_background.png"
$ReadmePath = Join-Path $Root "README.md"
$Utf8NoBom = New-Object System.Text.UTF8Encoding $false

function Get-ModSummaryFromReadme {
    param([string]$ModDir)
    $readme = Join-Path $ModDir "README.md"
    if (-not (Test-Path $readme)) { return "" }
    $lines = Get-Content $readme -Encoding UTF8
    foreach ($raw in $lines) {
        $line = $raw.Trim()
        if (-not $line) { continue }
        if ($line.StartsWith("#") -or $line.StartsWith(">") -or $line.StartsWith("!") -or $line.StartsWith("<")) { continue }
        return $line
    }
    return ""
}

# Load existing workshop_collection.json if present (preserves publishedFileId, title, description, custom summaries)
$existing = $null
$existingSummaries = @{}
if (Test-Path $CollectionPath) {
    $existing = Get-Content $CollectionPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($it in @($existing.items)) {
        if ($it.mod -and $it.summary) {
            $existingSummaries[[string]$it.mod] = [string]$it.summary
        }
    }
}

$collectionId = if ($existing -and $existing.publishedFileId) { [string]$existing.publishedFileId } else { "" }
$collectionTitle = if ($existing -and $existing.title) { [string]$existing.title } else { "WRSR Tweaks — Complete Collection" }
$collectionDesc = if ($existing -and $existing.description) { [string]$existing.description } else { "Complete collection of native x64 C++20 quality-of-life & logistics mods for Workers & Resources: Soviet Republic (RML & TesmioLoader)." }

# Discover all mods under mods/
$modDirs = Get-ChildItem (Join-Path $Root "mods") -Directory | Sort-Object Name
$items = [System.Collections.Generic.List[object]]::new()

foreach ($dir in $modDirs) {
    $modName = $dir.Name
    $gvPath = Join-Path $dir.FullName "game_versions.json"
    if (-not (Test-Path $gvPath)) { continue }

    $gv = Get-Content $gvPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $primary = @($gv.gameVersions | Where-Object { $_.isPrimary -eq $true } | Select-Object -First 1)
    if ($primary.Count -eq 0) {
        $primary = @($gv.gameVersions | Select-Object -First 1)
    }
    $pubId = if ($primary.Count -gt 0 -and $primary[0].publishedFileId) { [string]$primary[0].publishedFileId } else { "" }
    $publishWorkshop = if ($primary.Count -gt 0 -and $null -ne $primary[0].PSObject.Properties['publishWorkshop']) { [bool]$primary[0].publishWorkshop } else { $true }
    if (-not $publishWorkshop) { continue }

    $verInfo = & (Join-Path $PSScriptRoot "get_mod_version.ps1") -Mod $modName -AsObject
    $ver = [string]$verInfo.version

    $summary = if ($existingSummaries.ContainsKey($modName)) {
        $existingSummaries[$modName]
    } else {
        Get-ModSummaryFromReadme -ModDir $dir.FullName
    }

    $workshopTitle = if ($gv.workshopTitle) { [string]$gv.workshopTitle } else { $modName }
    $workshopUrl = if ($pubId -and $pubId -ne "0") { "https://steamcommunity.com/sharedfiles/filedetails/?id=$pubId" } else { "" }

    $items.Add([ordered]@{
        mod             = $modName
        workshopTitle   = $workshopTitle
        publishedFileId = $pubId
        version         = $ver
        workshopUrl     = $workshopUrl
        summary         = $summary
    })
}

$collectionObj = [ordered]@{
    appId           = "784150"
    publishedFileId = $collectionId
    collectionUrl   = if ($collectionId -and $collectionId -ne "0") { "https://steamcommunity.com/sharedfiles/filedetails/?id=$collectionId" } else { "" }
    title           = $collectionTitle
    description     = $collectionDesc
    items           = $items
}

$newJson = ($collectionObj | ConvertTo-Json -Depth 10) + "`n"

# Build Steam Workshop Collection BBCode description (workshop_collection_description.txt)
$bb = [System.Collections.Generic.List[string]]::new()
$bb.Add("[h1]$collectionTitle[/h1]")
$bb.Add("$collectionDesc")
$bb.Add("")
$bb.Add("Source code, documentation & standalone ZIP releases: [url=https://github.com/ZydraHydra/WorkersAndResources-Modding]ZydraHydra GitHub[/url]")
$bb.Add("")
$bb.Add("[b]Alpha Field Dispatch:[/b] All mods in this collection are early [b]Alpha[/b] native x64 plugins verified on WRSR 1.1.1.7 through 1.2.0.9. Back up your saves before testing and report any issues on GitHub!")
$bb.Add("")
$bb.Add("[h1]Included Mods ($($items.Count))[/h1]")
$bb.Add("[list]")
foreach ($it in $items) {
    $cleanSummary = ([string]$it.summary) -replace '\*\*([^*]+)\*\*', '[b]$1[/b]' -replace '\*([^*]+)\*', '[i]$1[/i]' -replace '`([^`]+)`', '[i]$1[/i]'
    if ($it.workshopUrl) {
        $bb.Add("[*] [url=$($it.workshopUrl)][b]$($it.workshopTitle)[/b][/url] (v$($it.version)) — $cleanSummary")
    } else {
        $bb.Add("[*] [b]$($it.workshopTitle)[/b] (v$($it.version)) — $cleanSummary")
    }
}
$bb.Add("[/list]")
$bb.Add("")
$bb.Add("[h1]Wait - What is a DLL Mod? (Step-by-Step Setup)[/h1]")
$bb.Add("Ordinary Steam Workshop mods only add buildings or vehicles. Because these mods add in-game [b]F8 / F9[/b] overlays and hook into game logic, they require a [b]Mod Loader[/b] alongside the game. Clicking Play in Steam alone will [b]not[/b] load DLL mods.")
$bb.Add("")
$bb.Add("Both [b]TesmioLoader (v0.4.5+)[/b] and [b]Republic Mod Loader (v1.0.1)[/b] automatically scan your subscribed Steam Workshop mods - [b]no manual file copying required[/b]:")
$bb.Add("")
$bb.Add("[b]Option A - TesmioLoader (Recommended - Mature & Updated for WRSR 1.2.0.9):[/b]")
$bb.Add("[list=1]")
$bb.Add("[*] Click [b]Subscribe to all[/b] on this collection.")
$bb.Add("[*] Download & extract [url=https://github.com/MaxLegend/TesmioLoader/releases]TesmioLoader (v0.4.5+)[/url] into your WRSR game directory ([url=https://maxlegend.github.io/TesmioLoaderDocs/#/getting-started]Docs[/url]).")
$bb.Add("[*] Run [b]tesmiolauncher.exe[/b], tick the mods you want, launch WRSR, and press [b]F9[/b] (or [b]F8[/b] for TradePriceLimits) in-game!")
$bb.Add("[/list]")
$bb.Add("[b]Option B - Republic Mod Loader (RML - All-on-Steam Alternative):[/b]")
$bb.Add("[list=1]")
$bb.Add("[*] Click [b]Subscribe to all[/b] on this collection.")
$bb.Add("[*] Also subscribe to [url=https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749]Republic Mod Loader (RML)[/url] ([url=https://github.com/Ultimate-Universe/WRSR-RepublicModLoader]GitHub[/url]).")
$bb.Add("[*] Run [b]RepublicModLoader.exe[/b] from:")
$bb.Add("[code]<Your Steam Library>\steamapps\workshop\content\784150\3787969749\RepublicModLoader.exe[/code]")
$bb.Add("[*] Tick the mods you want, click [b]Launch WRSR[/b], and press [b]F9[/b] (or [b]F8[/b]) in-game!")
$bb.Add("[/list]")
$newDesc = ($bb -join "`n") + "`n"

# Build README.md collection block
$mdLines = [System.Collections.Generic.List[string]]::new()
$mdLines.Add("<!-- COLLECTION_MODS_START -->")
if ($collectionId -and $collectionId -ne "0") {
    $mdLines.Add("> **Steam Workshop Collection:** Subscribe to all mods at once via the [**$collectionTitle**](https://steamcommunity.com/sharedfiles/filedetails/?id=$collectionId).")
    $mdLines.Add("")
}
$mdLines.Add("| Preview | Mod | Status | Description |")
$mdLines.Add("|---|---|---|---|")
foreach ($it in $items) {
    $m = $it.mod
    $preview = "<img src=`"mods/$m/steamcmd/previewimage.png`" width=`"96`" alt=`"$m`">"
    $modLink = "[``$m``](mods/$m)"
    $status = "v$($it.version) (Alpha)"
    $links = if ($it.workshopUrl) {
        "([Workshop]($($it.workshopUrl)) · [Releases](https://github.com/ZydraHydra/WorkersAndResources-Modding/releases))"
    } else {
        "([Releases](https://github.com/ZydraHydra/WorkersAndResources-Modding/releases))"
    }
    $desc = if ($it.summary) { "$($it.summary) $links" } else { $links }
    $mdLines.Add("| $preview | $modLink | $status | $desc |")
}
$mdLines.Add("<!-- COLLECTION_MODS_END -->")
$newBlock = $mdLines -join "`n"

$readmeRaw = Get-Content $ReadmePath -Raw -Encoding UTF8
$hasMarkers = $readmeRaw -match '(?s)<!-- COLLECTION_MODS_START -->.*?<!-- COLLECTION_MODS_END -->'
if ($hasMarkers) {
    $updatedReadme = [regex]::Replace($readmeRaw, '(?s)<!-- COLLECTION_MODS_START -->.*?<!-- COLLECTION_MODS_END -->', $newBlock)
} else {
    $updatedReadme = $readmeRaw
}

function Normalize-JsonForCompare {
    param([string]$RawJson)
    $c = ($RawJson | ConvertFrom-Json | ConvertTo-Json -Depth 10 -Compress)
    return ($c -replace '\\u0026', '&' -replace '\\u003c', '<' -replace '\\u003e', '>' -replace '\\u0027', "'")
}

$jsonChanged = $true
if (Test-Path $CollectionPath) {
    $oldJson = Get-Content $CollectionPath -Raw -Encoding UTF8
    try {
        $jsonChanged = ((Normalize-JsonForCompare $oldJson) -ne (Normalize-JsonForCompare $newJson))
    } catch {
        $jsonChanged = (($oldJson -replace "`r`n", "`n") -ne ($newJson -replace "`r`n", "`n"))
    }
}
$descChanged = $true
if (Test-Path $DescPath) {
    $oldDesc = (Get-Content $DescPath -Raw -Encoding UTF8) -replace "`r`n", "`n"
    $descChanged = ($oldDesc -ne ($newDesc -replace "`r`n", "`n"))
}
$readmeChanged = $hasMarkers -and (($readmeRaw -replace "`r`n", "`n") -ne ($updatedReadme -replace "`r`n", "`n"))

if ($CheckOnly) {
    if ($jsonChanged -or $descChanged -or $readmeChanged) {
        Write-Host "Collection files are out of sync with mods/*/game_versions.json:" -ForegroundColor Red
        if ($jsonChanged)   { Write-Host "  - workshop_collection.json needs update" -ForegroundColor Red }
        if ($descChanged)   { Write-Host "  - workshop_collection_description.txt needs update" -ForegroundColor Red }
        if ($readmeChanged) { Write-Host "  - README.md needs update" -ForegroundColor Red }
        Write-Host "Run '.\scripts\sync_collection.ps1' to update." -ForegroundColor Yellow
        exit 1
    }
    Write-Host "✓ workshop_collection.json, workshop_collection_description.txt, and README.md are in sync ($($items.Count) mods)." -ForegroundColor Green
} else {
    if ($jsonChanged) {
        [System.IO.File]::WriteAllText($CollectionPath, $newJson, $Utf8NoBom)
        Write-Host "✓ Updated workshop_collection.json ($($items.Count) mods)." -ForegroundColor Green
    } else {
        Write-Host "✓ workshop_collection.json already up to date ($($items.Count) mods)." -ForegroundColor DarkGray
    }
    if ($descChanged) {
        [System.IO.File]::WriteAllText($DescPath, $newDesc, $Utf8NoBom)
        Write-Host "✓ Updated workshop_collection_description.txt." -ForegroundColor Green
    } else {
        Write-Host "✓ workshop_collection_description.txt already up to date." -ForegroundColor DarkGray
    }
    if ($readmeChanged) {
        [System.IO.File]::WriteAllText($ReadmePath, $updatedReadme, $Utf8NoBom)
        Write-Host "✓ Updated README.md collection table." -ForegroundColor Green
    }
    & (Join-Path $PSScriptRoot "generate_collection_preview.ps1") -OutPath $PreviewPath -BackgroundOutPath $BackgroundPath
}

# Optional live Steam Workshop Collection check via public Steam Web API
if ($CheckSteam) {
    $summaryLines = [System.Collections.Generic.List[string]]::new()
    $summaryLines.Add("### Steam Workshop Collection Sync")

    if (-not $collectionId -or $collectionId -eq "0") {
        $msg = "Steam Workshop Collection ID is not set yet in ``workshop_collection.json`` (``publishedFileId``). Create the collection on Steam Community and set its ID to enable live sync verification."
        Write-Host "⚠ $msg" -ForegroundColor Yellow
        $summaryLines.Add("- ⚠ $msg")
    } else {
        $uri = "https://api.steampowered.com/ISteamRemoteStorage/GetCollectionDetails/v1/"
        try {
            $resp = Invoke-RestMethod -Uri $uri -Method Post -Body @{
                "collectioncount"    = 1
                "publishedfileids[0]" = $collectionId
            } -TimeoutSec 15

            $details = $resp.response.collectiondetails | Select-Object -First 1
            if (-not $details -or $details.result -ne 1) {
                $errMsg = "Could not fetch Steam Collection $collectionId (result=$($details.result)). Ensure the collection is Public or Unlisted."
                Write-Host "⚠ $errMsg" -ForegroundColor Yellow
                $summaryLines.Add("- ⚠ $errMsg")
                if ($Strict) { exit 1 }
            } else {
                $steamIds = @($details.children | ForEach-Object { [string]$_.publishedfileid })
                $repoItems = @($items | Where-Object { $_.publishedFileId -and $_.publishedFileId -ne "0" })
                $repoIds = @($repoItems | ForEach-Object { [string]$_.publishedFileId })

                $missingFromSteam = @($repoItems | Where-Object { $steamIds -notcontains $_.publishedFileId })
                $extraInSteam = @($steamIds | Where-Object { $repoIds -notcontains $_ })

                if ($missingFromSteam.Count -eq 0 -and $extraInSteam.Count -eq 0) {
                    $okMsg = "Steam Collection [$collectionId](https://steamcommunity.com/sharedfiles/filedetails/?id=$collectionId) is in sync with all $($repoItems.Count) repo mods."
                    Write-Host "✓ $okMsg" -ForegroundColor Green
                    $summaryLines.Add("- ✓ $okMsg")
                } else {
                    Write-Host "⚠ Steam Collection $collectionId is out of sync:" -ForegroundColor Yellow
                    $summaryLines.Add("- ⚠ Steam Collection [$collectionId](https://steamcommunity.com/sharedfiles/filedetails/?id=$collectionId) needs manual update:")
                    foreach ($m in $missingFromSteam) {
                        Write-Host "  + Add to Steam Collection: $($m.mod) ($($m.publishedFileId)) — $($m.workshopUrl)" -ForegroundColor Yellow
                        $summaryLines.Add("  - **Add:** ``$($m.mod)`` ([Workshop $($m.publishedFileId)]($($m.workshopUrl)))")
                    }
                    foreach ($e in $extraInSteam) {
                        Write-Host "  - Remove from Steam Collection: $e (https://steamcommunity.com/sharedfiles/filedetails/?id=$e)" -ForegroundColor Yellow
                        $summaryLines.Add("  - **Remove:** [Workshop $e](https://steamcommunity.com/sharedfiles/filedetails/?id=$e)")
                    }
                    if ($Strict) { exit 1 }
                }
            }
        } catch {
            $errMsg = "Steam API check failed: $($_.Exception.Message)"
            Write-Host "⚠ $errMsg" -ForegroundColor Yellow
            $summaryLines.Add("- ⚠ $errMsg")
            if ($Strict) { exit 1 }
        }
    }

    if ($SummaryFile) {
        ($summaryLines -join "`n") + "`n" | Out-File -FilePath $SummaryFile -Append -Encoding utf8
    }
}
