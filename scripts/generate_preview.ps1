# ============================================================================
# generate_preview.ps1 — Generate standardized 700x700 WRSR previewimage.png
# ============================================================================
# Reads mods/<Mod>/steamcmd/preview.json and renders a uniform Soviet-industrial
# dark-gold-crimson UI mockup previewimage.png matching the suite style.
#
# Usage: .\scripts\generate_preview.ps1 -Mod CODispatchControl
# ============================================================================
param(
    [Parameter(Mandatory = $true)]
    [string]$Mod
)

$ErrorActionPreference = "Stop"
$Root     = Split-Path $PSScriptRoot -Parent
$ModDir   = Join-Path $Root "mods\$Mod"
$SpecPath = Join-Path $ModDir "steamcmd\preview.json"
$OutPath  = Join-Path $ModDir "steamcmd\previewimage.png"

if (-not (Test-Path $SpecPath)) {
    throw "Preview spec not found: $SpecPath"
}

$spec = Get-Content $SpecPath -Raw -Encoding UTF8 | ConvertFrom-Json

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

# Palette
$cBg          = [System.Drawing.Color]::FromArgb(255, 18, 17, 13)
$cOuterBorder = [System.Drawing.Color]::FromArgb(255, 200, 154, 58)
$cCrimson     = [System.Drawing.Color]::FromArgb(255, 152, 32, 28)
$cWhite       = [System.Drawing.Color]::FromArgb(255, 245, 242, 232)
$cMuted       = [System.Drawing.Color]::FromArgb(255, 170, 162, 138)
$cGold        = [System.Drawing.Color]::FromArgb(255, 214, 166, 69)
$cPillBg      = [System.Drawing.Color]::FromArgb(255, 26, 24, 18)
$cWinBg       = [System.Drawing.Color]::FromArgb(255, 22, 20, 15)
$cWinHeader   = [System.Drawing.Color]::FromArgb(255, 34, 30, 22)
$cTabInactive = [System.Drawing.Color]::FromArgb(255, 42, 38, 28)
$cTabActive   = [System.Drawing.Color]::FromArgb(255, 138, 102, 40)
$cGreen       = [System.Drawing.Color]::FromArgb(255, 128, 198, 92)
$cSliderBg    = [System.Drawing.Color]::FromArgb(255, 46, 42, 32)
$cSliderGrab  = [System.Drawing.Color]::FromArgb(255, 206, 162, 72)
$cBtnBg       = [System.Drawing.Color]::FromArgb(255, 36, 32, 24)
$cSaveBtnBg   = [System.Drawing.Color]::FromArgb(255, 124, 92, 36)

# Background & Outer Gold Frame
$g.Clear($cBg)
$outerPen = New-Object System.Drawing.Pen $cOuterBorder, 2
$g.DrawRectangle($outerPen, 8, 8, $W - 17, $H - 17)

# Fonts
$fBadge    = New-Object System.Drawing.Font "Segoe UI", 11, ([System.Drawing.FontStyle]::Bold)
$fTitle    = New-Object System.Drawing.Font "Segoe UI", 34, ([System.Drawing.FontStyle]::Bold)
$fSubtitle = New-Object System.Drawing.Font "Segoe UI", 12.5, ([System.Drawing.FontStyle]::Regular)
$fPill     = New-Object System.Drawing.Font "Segoe UI", 9.5, ([System.Drawing.FontStyle]::Bold)
$fMono     = New-Object System.Drawing.Font "Consolas", 10.5, ([System.Drawing.FontStyle]::Regular)
$fMonoBold = New-Object System.Drawing.Font "Consolas", 10.5, ([System.Drawing.FontStyle]::Bold)
$fMonoSm   = New-Object System.Drawing.Font "Consolas", 9.5, ([System.Drawing.FontStyle]::Regular)
$fTagline  = New-Object System.Drawing.Font "Segoe UI", 13.5, ([System.Drawing.FontStyle]::Bold)
$fFooter   = New-Object System.Drawing.Font "Segoe UI", 10.5, ([System.Drawing.FontStyle]::Regular)

$sfCenter = New-Object System.Drawing.StringFormat
$sfCenter.Alignment = [System.Drawing.StringAlignment]::Center
$sfCenter.LineAlignment = [System.Drawing.StringAlignment]::Center

$sfLeft = New-Object System.Drawing.StringFormat
$sfLeft.Alignment = [System.Drawing.StringAlignment]::Near
$sfLeft.LineAlignment = [System.Drawing.StringAlignment]::Center

# Top Badges
$bCrimson = New-Object System.Drawing.SolidBrush $cCrimson
$bWhite   = New-Object System.Drawing.SolidBrush $cWhite
$bMuted   = New-Object System.Drawing.SolidBrush $cMuted
$bGold    = New-Object System.Drawing.SolidBrush $cGold

$pLeftBadge = New-RoundedRectPath 28 26 156 28 6
$g.FillPath($bCrimson, $pLeftBadge)
$g.DrawString([string]$spec.gameVersionBadge, $fBadge, $bWhite, (New-Rect 28 26 156 28), $sfCenter)

$rx = $W - 200
$pRightBadge = New-RoundedRectPath $rx 26 172 28 6
$g.FillPath($bCrimson, $pRightBadge)
$g.DrawString([string]$spec.hotkeyBadge, $fBadge, $bWhite, (New-Rect $rx 26 172 28), $sfCenter)

# Title & Subtitle
$g.DrawString(([string]$spec.title).ToUpper(), $fTitle, $bWhite, (New-Rect 28 72 ($W - 56) 54), $sfLeft)
$g.DrawString([string]$spec.subtitle, $fSubtitle, $bMuted, (New-Rect 30 128 ($W - 60) 26), $sfLeft)

# 3 Feature Pills
$pills = @($spec.pills)
$pillW = 198
$pillGap = 19
$pillX = 28
$pillY = 166
$bPillBg = New-Object System.Drawing.SolidBrush $cPillBg
$penGold1 = New-Object System.Drawing.Pen $cGold, 1.2
for ($i = 0; $i -lt [Math]::Min(3, $pills.Count); $i++) {
    $x = $pillX + $i * ($pillW + $pillGap)
    $pp = New-RoundedRectPath $x $pillY $pillW 28 6
    $g.FillPath($bPillBg, $pp)
    $g.DrawPath($penGold1, $pp)
    $g.DrawString(([string]$pills[$i]).ToUpper(), $fPill, $bGold, (New-Rect $x $pillY $pillW 28), $sfCenter)
}

# Embedded Overlay Window Mockup
$winX = 28; $winY = 212; $winW = $W - 56; $winH = 376
$bWinBg = New-Object System.Drawing.SolidBrush $cWinBg
$bWinHeader = New-Object System.Drawing.SolidBrush $cWinHeader
$g.FillRectangle($bWinBg, $winX, $winY, $winW, $winH)
$g.DrawRectangle($penGold1, $winX, $winY, $winW, $winH)
$g.FillRectangle($bWinHeader, $winX + 1, $winY + 1, $winW - 2, 28)

$g.DrawString([string]$spec.windowTitle, $fMonoBold, $bWhite, (New-Rect ($winX + 12) $winY 300 28), $sfLeft)
$g.DrawString("x", $fMono, $bMuted, (New-Rect ($winX + $winW - 24) $winY 16 28), $sfCenter)

# Tabs
$tabs = @($spec.tabs)
$tabX = $winX + 14
$tabY = $winY + 38
$bTabInact = New-Object System.Drawing.SolidBrush $cTabInactive
$bTabAct   = New-Object System.Drawing.SolidBrush $cTabActive
foreach ($t in $tabs) {
    $tw = [Math]::Max(110, ([string]$t.label).Length * 9 + 24)
    $tb = if ($t.active) { $bTabAct } else { $bTabInact }
    $tf = if ($t.active) { $bWhite } else { $bMuted }
    $g.FillRectangle($tb, $tabX, $tabY, $tw, 24)
    $g.DrawString([string]$t.label, $fMonoSm, $tf, (New-Rect $tabX $tabY $tw 24), $sfCenter)
    $tabX += $tw + 6
}
$g.DrawLine($penGold1, $winX + 14, $tabY + 28, $winX + $winW - 14, $tabY + 28)

# Hook status & description
$bGreen = New-Object System.Drawing.SolidBrush $cGreen
$g.DrawString([string]$spec.hookStatus, $fMonoSm, $bGreen, (New-Rect ($winX + 14) ($tabY + 34) ($winW - 28) 22), $sfLeft)
$g.DrawString([string]$spec.windowDesc, $fMonoSm, $bWhite, (New-Rect ($winX + 14) ($tabY + 58) ($winW - 28) 22), $sfLeft)

# Sliders
$bSliderBg = New-Object System.Drawing.SolidBrush $cSliderBg
$bSliderGrab = New-Object System.Drawing.SolidBrush $cSliderGrab
$curY = $tabY + 90
foreach ($s in @($spec.sliders)) {
    $slW = 360
    $slH = 22
    $slX = $winX + 14
    $g.FillRectangle($bSliderBg, $slX, $curY, $slW, $slH)
    $frac = [Math]::Max(0.0, [Math]::Min(1.0, [double]$s.fraction))
    $grabX = $slX + [int](($slW - 12) * $frac)
    $g.FillRectangle($bSliderGrab, $grabX, $curY, 10, $slH)
    $g.DrawString([string]$s.valueText, $fMonoSm, $bWhite, (New-Rect $slX $curY $slW $slH), $sfCenter)
    $g.DrawString([string]$s.label, $fMonoSm, $bWhite, (New-Rect ($slX + $slW + 14) $curY 240 $slH), $sfLeft)
    $curY += 32
}

# Presets
$curY += 10
$g.DrawString("Quick Presets:", $fMonoSm, $bMuted, (New-Rect ($winX + 14) $curY 200 20), $sfLeft)
$curY += 26
$btnX = $winX + 14
$bBtnBg = New-Object System.Drawing.SolidBrush $cBtnBg
$penMuted = New-Object System.Drawing.Pen $cMuted, 1
foreach ($p in @($spec.presets)) {
    $bw = [Math]::Max(74, ([string]$p).Length * 9 + 20)
    $g.FillRectangle($bBtnBg, $btnX, $curY, $bw, 26)
    $g.DrawRectangle($penMuted, $btnX, $curY, $bw, 26)
    $g.DrawString([string]$p, $fMonoSm, $bWhite, (New-Rect $btnX $curY $bw 26), $sfCenter)
    $btnX += $bw + 8
}

# Save Config + Counter
$curY += 42
$bSaveBtn = New-Object System.Drawing.SolidBrush $cSaveBtnBg
$g.FillRectangle($bSaveBtn, $winX + 14, $curY, 120, 28)
$g.DrawString("Save Config", $fMonoSm, $bWhite, (New-Rect ($winX + 14) $curY 120 28), $sfCenter)
$g.DrawString([string]$spec.footerCounter, $fMonoSm, $bGold, (New-Rect ($winX + 148) $curY 460 28), $sfLeft)

# Bottom Tagline & Footer
$g.DrawString([string]$spec.tagline, $fTagline, $bGold, (New-Rect 28 604 ($W - 56) 36), $sfCenter)
$g.DrawString([string]$spec.footer, $fFooter, $bMuted, (New-Rect 28 644 ($W - 56) 28), $sfCenter)

$bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()

Write-Host "✓ Generated 700x700 previewimage.png -> $OutPath" -ForegroundColor Green
