# ============================================================================
# generate_collection_preview.ps1 — Generate 700x700 Steam Collection banner
# ============================================================================
# Reads workshop_collection.json and renders workshop_collection_preview.png
# in the standardized Soviet-industrial dark-gold-crimson suite style.
#
# Usage: .\scripts\generate_collection_preview.ps1
# ============================================================================
param(
    [string]$OutPath = "",
    [string]$BackgroundOutPath = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$CollectionPath = Join-Path $Root "workshop_collection.json"
if (-not $OutPath) {
    $OutPath = Join-Path $Root "workshop_collection_preview.png"
}
if (-not $BackgroundOutPath) {
    $BackgroundOutPath = Join-Path $Root "workshop_collection_background.png"
}

if (-not (Test-Path $CollectionPath)) {
    throw "workshop_collection.json not found. Run .\scripts\sync_collection.ps1 first."
}

$col = Get-Content $CollectionPath -Raw -Encoding UTF8 | ConvertFrom-Json
$items = @($col.items)

Add-Type -AssemblyName System.Drawing

function New-Rect {
    param([float]$X, [float]$Y, [float]$W, [float]$H)
    return (New-Object System.Drawing.RectangleF $X, $Y, $W, $H)
}

function New-RoundedRectPath {
    param([float]$X, [float]$Y, [float]$W, [float]$H, [float]$R)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $R * 2
    $path.AddArc($X, $Y, $d, $d, 180, 90)
    $path.AddArc($X + $W - $d, $Y, $d, $d, 270, 90)
    $path.AddArc($X + $W - $d, $Y + $H - $d, $d, $d, 0, 90)
    $path.AddArc($X, $Y + $H - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

$W = 700
$H = 700
$bmp = New-Object System.Drawing.Bitmap $W, $H
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit

# Palette (matches scripts/generate_preview.ps1)
$cBg          = [System.Drawing.Color]::FromArgb(255, 18, 17, 13)
$cOuterBorder = [System.Drawing.Color]::FromArgb(255, 200, 154, 58)
$cCrimson     = [System.Drawing.Color]::FromArgb(255, 152, 32, 28)
$cWhite       = [System.Drawing.Color]::FromArgb(255, 245, 242, 232)
$cMuted       = [System.Drawing.Color]::FromArgb(255, 170, 162, 138)
$cGold        = [System.Drawing.Color]::FromArgb(255, 214, 166, 69)
$cPillBg      = [System.Drawing.Color]::FromArgb(255, 26, 24, 18)
$cWinBg       = [System.Drawing.Color]::FromArgb(255, 22, 20, 15)
$cWinHeader   = [System.Drawing.Color]::FromArgb(255, 34, 30, 22)
$cRowBg       = [System.Drawing.Color]::FromArgb(255, 30, 27, 20)
$cGreen       = [System.Drawing.Color]::FromArgb(255, 128, 198, 92)

$g.Clear($cBg)
$outerPen = New-Object System.Drawing.Pen $cOuterBorder, 2
$g.DrawRectangle($outerPen, 8, 8, $W - 17, $H - 17)

# Fonts
$fBadge    = New-Object System.Drawing.Font "Segoe UI", 10.5, ([System.Drawing.FontStyle]::Bold)
$fTitle    = New-Object System.Drawing.Font "Segoe UI", 33, ([System.Drawing.FontStyle]::Bold)
$fSubtitle = New-Object System.Drawing.Font "Segoe UI", 12, ([System.Drawing.FontStyle]::Regular)
$fPill     = New-Object System.Drawing.Font "Segoe UI", 9.5, ([System.Drawing.FontStyle]::Bold)
$fMono     = New-Object System.Drawing.Font "Consolas", 10, ([System.Drawing.FontStyle]::Regular)
$fMonoBold = New-Object System.Drawing.Font "Consolas", 10, ([System.Drawing.FontStyle]::Bold)
$fTagline  = New-Object System.Drawing.Font "Segoe UI", 13, ([System.Drawing.FontStyle]::Bold)
$fFooter   = New-Object System.Drawing.Font "Segoe UI", 10, ([System.Drawing.FontStyle]::Regular)

$sfCenter = New-Object System.Drawing.StringFormat
$sfCenter.Alignment = [System.Drawing.StringAlignment]::Center
$sfCenter.LineAlignment = [System.Drawing.StringAlignment]::Center

$sfLeft = New-Object System.Drawing.StringFormat
$sfLeft.Alignment = [System.Drawing.StringAlignment]::Near
$sfLeft.LineAlignment = [System.Drawing.StringAlignment]::Center

$sfRight = New-Object System.Drawing.StringFormat
$sfRight.Alignment = [System.Drawing.StringAlignment]::Far
$sfRight.LineAlignment = [System.Drawing.StringAlignment]::Center

$bCrimson = New-Object System.Drawing.SolidBrush $cCrimson
$bWhite   = New-Object System.Drawing.SolidBrush $cWhite
$bMuted   = New-Object System.Drawing.SolidBrush $cMuted
$bGold    = New-Object System.Drawing.SolidBrush $cGold
$bPillBg  = New-Object System.Drawing.SolidBrush $cPillBg
$bWinBg   = New-Object System.Drawing.SolidBrush $cWinBg
$bWinHdr  = New-Object System.Drawing.SolidBrush $cWinHeader
$bRowBg   = New-Object System.Drawing.SolidBrush $cRowBg
$bGreen   = New-Object System.Drawing.SolidBrush $cGreen
$penGold1 = New-Object System.Drawing.Pen $cGold, 1.2
$penDim   = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 64, 56, 40)), 1

# Top Badges
$pLeftBadge = New-RoundedRectPath 28 26 200 28 6
$g.FillPath($bCrimson, $pLeftBadge)
$g.DrawString("WRSR 1.1.1.7 - 1.2.0.9", $fBadge, $bWhite, (New-Rect 28 26 200 28), $sfCenter)

$rx = $W - 210
$pRightBadge = New-RoundedRectPath $rx 26 182 28 6
$g.FillPath($bCrimson, $pRightBadge)
$g.DrawString("$($items.Count) MODS | F8 & F9", $fBadge, $bWhite, (New-Rect $rx 26 182 28), $sfCenter)

# Title & Subtitle
$g.DrawString("WRSR TWEAKS SUITE", $fTitle, $bWhite, (New-Rect 28 68 ($W - 56) 52), $sfLeft)
$g.DrawString("Complete Native x64 Plugin Collection for RML & TesmioLoader", $fSubtitle, $bMuted, (New-Rect 30 122 ($W - 60) 24), $sfLeft)

# 3 Feature Pills
$pills = @("SHARED F9 OVERLAY", "AUTO VERSION DETECT", "SAVE-SAFE HOOKS")
$pillW = 198
$pillGap = 19
$pillX = 28
$pillY = 158
for ($i = 0; $i -lt $pills.Count; $i++) {
    $px = $pillX + $i * ($pillW + $pillGap)
    $pp = New-RoundedRectPath $px $pillY $pillW 30 5
    $g.FillPath($bPillBg, $pp)
    $g.DrawPath($penGold1, $pp)
    $g.DrawString($pills[$i], $fPill, $bGold, (New-Rect $px $pillY $pillW 30), $sfCenter)
}

# Center Suite Window
$winX = 28; $winY = 204; $winW = 644; $winH = 408
$pWin = New-RoundedRectPath $winX $winY $winW $winH 6
$g.FillPath($bWinBg, $pWin)
$g.DrawPath($penGold1, $pWin)

# Window Header
$g.FillRectangle($bWinHdr, $winX + 1, $winY + 1, $winW - 2, 34)
$g.DrawLine($penDim, $winX, $winY + 35, $winX + $winW, $winY + 35)
$g.DrawString("WRSR Tweaks — Included Plugins ($($items.Count))", $fMonoBold, $bGold, (New-Rect ($winX + 14) $winY ($winW - 28) 34), $sfLeft)

# Mod rows
$maxRows = [Math]::Min($items.Count, 8)
$rowStartY = $winY + 46
$rowH = 40
$rowGap = 6
for ($i = 0; $i -lt $maxRows; $i++) {
    $it = $items[$i]
    $ry = $rowStartY + $i * ($rowH + $rowGap)
    $pRow = New-RoundedRectPath ($winX + 12) $ry ($winW - 24) $rowH 4
    $g.FillPath($bRowBg, $pRow)
    $g.DrawPath($penDim, $pRow)

    # Green status dot
    $g.FillEllipse($bGreen, ($winX + 24), ($ry + 15), 10, 10)

    # Mod name & short title
    $g.DrawString([string]$it.mod, $fMonoBold, $bWhite, (New-Rect ($winX + 44) $ry 185 $rowH), $sfLeft)

    $shortTitle = ([string]$it.workshopTitle) -replace '^WRSR Tweaks:\s*', ''
    $g.DrawString($shortTitle, $fMono, $bMuted, (New-Rect ($winX + 230) $ry 315 $rowH), $sfLeft)

    $verStr = "v$($it.version)"
    $g.DrawString($verStr, $fMonoBold, $bGold, (New-Rect ($winX + 545) $ry 75 $rowH), $sfRight)
}

if ($items.Count -gt $maxRows) {
    $extra = $items.Count - $maxRows
    $g.DrawString("+ $extra more plugin(s) in collection", $fMono, $bMuted, (New-Rect ($winX + 14) ($winY + $winH - 28) ($winW - 28) 22), $sfCenter)
}

# Bottom Tagline & Footer
$g.DrawString("ALL-IN-ONE QUALITY-OF-LIFE & LOGISTICS SUITE", $fTagline, $bGold, (New-Rect 28 624 ($W - 56) 28), $sfCenter)
$g.DrawString("Subscribe once  |  Requires TesmioLoader (v0.4.5+) or Republic Mod Loader (RML)", $fFooter, $bMuted, (New-Rect 28 654 ($W - 56) 24), $sfCenter)

$bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()

Write-Host "✓ Generated collection preview: $OutPath ($($items.Count) mods)" -ForegroundColor Green

# ----------------------------------------------------------------------------
# Generate 1920x1080 widescreen Steam Collection background image
# ----------------------------------------------------------------------------
$BW = 1920
$BH = 1080
$bgBmp = New-Object System.Drawing.Bitmap $BW, $BH
$bg = [System.Drawing.Graphics]::FromImage($bgBmp)
$bg.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$bg.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit

# Base dark gradient
$bgRect = New-Object System.Drawing.Rectangle 0, 0, $BW, $BH
$gradBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    $bgRect,
    [System.Drawing.Color]::FromArgb(255, 22, 19, 14),
    [System.Drawing.Color]::FromArgb(255, 12, 11, 8),
    [System.Drawing.Drawing2D.LinearGradientMode]::Vertical
)
$bg.FillRectangle($gradBrush, $bgRect)
$gradBrush.Dispose()

# Subtle industrial blueprint grid
$gridPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(18, 200, 154, 58)), 1
for ($gx = 0; $gx -lt $BW; $gx += 64) {
    $bg.DrawLine($gridPen, $gx, 0, $gx, $BH)
}
for ($gy = 0; $gy -lt $BH; $gy += 64) {
    $bg.DrawLine($gridPen, 0, $gy, $BW, $gy)
}
$gridPen.Dispose()

# Subtle crimson & gold side accent bars (kept away from center content area)
$sideGoldPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(70, 200, 154, 58)), 2
$sideCrimsonBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(140, 152, 32, 28))
$bg.FillRectangle($sideCrimsonBrush, 0, 0, $BW, 6)
$bg.FillRectangle($sideCrimsonBrush, 0, $BH - 6, $BW, 6)
$bg.DrawRectangle($sideGoldPen, 28, 28, $BW - 57, $BH - 57)

# Left & right subtle vertical industrial accent stripes
$stripeBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(22, 214, 166, 69))
$bg.FillRectangle($stripeBrush, 56, 60, 8, $BH - 120)
$bg.FillRectangle($stripeBrush, $BW - 64, 60, 8, $BH - 120)

# Subtle corner watermarks (visible outside Steam's centered 940px content column)
$fBgTag = New-Object System.Drawing.Font "Segoe UI", 13, ([System.Drawing.FontStyle]::Bold)
$fBgSub = New-Object System.Drawing.Font "Consolas", 11, ([System.Drawing.FontStyle]::Regular)
$bBgGold = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(110, 214, 166, 69))
$bBgMuted = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(85, 170, 162, 138))

$bg.DrawString("WRSR TWEAKS SUITE", $fBgTag, $bBgGold, (New-Rect 84 56 320 28), $sfLeft)
$bg.DrawString("NATIVE X64 PLUGINS  |  RML & TESMIOLOADER", $fBgSub, $bBgMuted, (New-Rect 84 86 420 24), $sfLeft)

$bg.DrawString("WORKERS & RESOURCES: SOVIET REPUBLIC", $fBgTag, $bBgGold, (New-Rect ($BW - 484) 56 400 28), $sfRight)
$bg.DrawString("WRSR 1.1.1.7 - 1.2.0.9  |  F8 & F9", $fBgSub, $bBgMuted, (New-Rect ($BW - 484) 86 400 24), $sfRight)

$bg.DrawString("GITHUB.COM/ZYDRAHYDRA/WORKERSANDRESOURCES-MODDING", $fBgSub, $bBgMuted, (New-Rect 84 ($BH - 86) 560 24), $sfLeft)
$bg.DrawString("$($items.Count) COLLECTION MODS", $fBgSub, $bBgGold, (New-Rect ($BW - 384) ($BH - 86) 300 24), $sfRight)

# Darken center 1040px column slightly so Steam's foreground collection UI pops cleanly
$centerOverlay = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(95, 12, 11, 8))
$bg.FillRectangle($centerOverlay, 440, 0, 1040, $BH)

$centerOverlay.Dispose()
$sideGoldPen.Dispose()
$sideCrimsonBrush.Dispose()
$stripeBrush.Dispose()
$bBgGold.Dispose()
$bBgMuted.Dispose()
$fBgTag.Dispose()
$fBgSub.Dispose()

$bgBmp.Save($BackgroundOutPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bg.Dispose()
$bgBmp.Dispose()

Write-Host "✓ Generated collection background: $BackgroundOutPath (1920x1080)" -ForegroundColor Green
