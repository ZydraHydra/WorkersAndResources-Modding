# ============================================================================
# build.ps1 — Configure & compile WRSR C++ plugins (per mod + game version)
# ============================================================================
# Usage:
#   .\scripts\build.ps1                                   # All mods, default/all game versions
#   .\scripts\build.ps1 -Mod TradePriceLimits
#   .\scripts\build.ps1 -Mod TradePriceLimits -GameVersion 1.1.1.7
#   .\scripts\build.ps1 -Mod TradePriceLimits -GameVersion all
# ============================================================================
param(
    [string]$Mod = "all",
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string]$GameVersion = "",
    [string]$VersionOverride = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent

# Locate cmake
$cmake = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue
if (-not $cmake) {
    foreach ($c in @("C:\Program Files\CMake\bin\cmake.exe", "$env:LOCALAPPDATA\Programs\CMake\bin\cmake.exe")) {
        if (Test-Path $c) { $cmake = $c; break }
    }
    if (-not $cmake) { throw "cmake not found — install Kitware.CMake or add it to PATH" }
}

# Locate ninja
$ninja = Get-Command ninja -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue
if (-not $ninja) {
    $candidates = @("$env:LOCALAPPDATA\Microsoft\WinGet\Links\ninja.exe", "C:\Program Files\ninja\ninja.exe")
    $wingetPkg  = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages"
    if (Test-Path $wingetPkg) {
        $found = Get-ChildItem $wingetPkg -Recurse -Filter "ninja.exe" -ErrorAction SilentlyContinue |
                 Select-Object -First 1 -ExpandProperty FullName
        if ($found) { $candidates = @($found) + $candidates }
    }
    foreach ($c in $candidates) { if (Test-Path $c) { $ninja = $c; break } }
    if (-not $ninja) { throw "ninja not found — install Ninja-build.Ninja or add it to PATH" }
}

# Locate MSVC x64 environment via vswhere
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found — install Visual Studio 2022 or Build Tools" }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "MSVC x64 C++ Build Tools not found" }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at: $vcvars" }

$ninjaDir  = Split-Path $ninja -Parent
$depsDir   = (Join-Path $Root "build\_deps") -replace '\\', '/'

# Resolve target mod list and game version matrix
$modNames = if ($Mod -eq "all") {
    Get-ChildItem (Join-Path $Root "mods") -Directory | Select-Object -ExpandProperty Name
} else {
    @($Mod)
}

function Invoke-BuildSlice {
    param(
        [string]$TargetArg,
        [string]$GvId,
        [string]$Label,
        [string]$ModVerOverride
    )

    $BuildDir = Join-Path $Root "build\$($Config.ToLower())\$GvId"
    $verArg   = if ($ModVerOverride) { "-DWRSR_MOD_VERSION_OVERRIDE=`"$ModVerOverride`"" } else { "" }

    $script = @"
@call "$vcvars" > nul 2>&1
set PATH=$ninjaDir;%PATH%
"$cmake" -S "$Root" -B "$BuildDir" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_MAKE_PROGRAM="$ninja" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DFETCHCONTENT_BASE_DIR="$depsDir" -DWRSR_GAME_VERSION="$GvId" $verArg
if errorlevel 1 exit /b 1
"$cmake" --build "$BuildDir" $TargetArg -j $([Environment]::ProcessorCount)
if errorlevel 1 exit /b 1
"@

    $tmpBat = Join-Path $env:TEMP "wrsr_build_$([guid]::NewGuid().ToString('N')).bat"
    [System.IO.File]::WriteAllText($tmpBat, $script, [System.Text.Encoding]::ASCII)

    Write-Host "=== Configuring & Building ($Label | WRSR $GvId | $Config) ===" -ForegroundColor Cyan
    & cmd.exe /c "`"$tmpBat`""
    $exitCode = $LASTEXITCODE
    Remove-Item $tmpBat -ErrorAction SilentlyContinue

    if ($exitCode -ne 0) {
        throw "Build failed for $Label ($GvId) with exit code $exitCode"
    }

    Write-Host "`n✓ BUILD SUCCEEDED ($Label | WRSR $GvId)" -ForegroundColor Green
    Get-ChildItem (Join-Path $BuildDir "mods") -Recurse -Filter "*.dll" -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host ("  {0} ({1:N0} bytes)" -f $_.FullName, $_.Length) }
}

if ($GameVersion -eq "all") {
    # Build every declared gameVersion across the selected mods
    foreach ($m in $modNames) {
        $info = & (Join-Path $PSScriptRoot "get_mod_version.ps1") -Mod $m -VersionOverride $VersionOverride -AsObject
        foreach ($gv in $info.gameVersions) {
            Invoke-BuildSlice -TargetArg "--target $m" -GvId $gv.id -Label "$m v$($info.version)" -ModVerOverride $info.version
        }
    }
} else {
    if (-not $GameVersion) {
        # Use first selected mod's defaultGameVersion (fallback "latest")
        $firstInfo = & (Join-Path $PSScriptRoot "get_mod_version.ps1") -Mod ($modNames[0]) -VersionOverride $VersionOverride -AsObject
        $GameVersion = $firstInfo.defaultGameVersion
    }

    if ($Mod -eq "all") {
        Invoke-BuildSlice -TargetArg "" -GvId $GameVersion -Label "all" -ModVerOverride $VersionOverride
    } else {
        $info = & (Join-Path $PSScriptRoot "get_mod_version.ps1") -Mod $Mod -VersionOverride $VersionOverride -AsObject
        Invoke-BuildSlice -TargetArg "--target $Mod" -GvId $GameVersion -Label "$Mod v$($info.version)" -ModVerOverride $info.version
    }
}
