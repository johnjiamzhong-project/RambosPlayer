# RambosPlayer packaging script
# Usage: .\package.ps1 [-Version "1.0.0"]

param(
    [string]$Version = "1.0.0"
)

# ── Path config ───────────────────────────────────────
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildExe    = "$ProjectRoot\build\Release\RambosPlayer.exe"
$DistDir     = "$PSScriptRoot\dist"
$ZipOut      = "$PSScriptRoot\RambosPlayer-v$Version.zip"

$QtBin       = "E:\Qt\Qt5.14.2\5.14.2\msvc2017_64\bin"
$VcpkgBin    = "E:\vcpkg\installed\x64-windows\bin"
# ──────────────────────────────────────────────────────

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Log($msg) { Write-Host "[package] $msg" -ForegroundColor Cyan }
function Die($msg) { Write-Host "[ERROR] $msg" -ForegroundColor Red; exit 1 }

# 1. Check exe exists
if (-not (Test-Path $BuildExe)) {
    Die "Not found: $BuildExe -- run: cmake --build build --config Release"
}

# 2. Clean and recreate dist
Log "Cleaning dist..."
if (Test-Path $DistDir) { Remove-Item $DistDir -Recurse -Force }
New-Item -ItemType Directory -Path $DistDir | Out-Null

# 3. Copy exe
Log "Copying RambosPlayer.exe..."
Copy-Item $BuildExe $DistDir

# 4. Collect Qt runtime via windeployqt
Log "Running windeployqt..."
$windeployqt = "$QtBin\windeployqt.exe"
if (-not (Test-Path $windeployqt)) { Die "windeployqt not found: $windeployqt" }
& $windeployqt "$DistDir\RambosPlayer.exe" --no-translations --no-system-d3d-compiler --no-opengl-sw
if ($LASTEXITCODE -ne 0) { Die "windeployqt failed" }

# 5. Copy FFmpeg DLLs
Log "Copying FFmpeg DLLs..."
$ffmpegDlls = @(
    "avcodec-*.dll",
    "avformat-*.dll",
    "avutil-*.dll",
    "avfilter-*.dll",
    "swresample-*.dll",
    "swscale-*.dll",
    "postproc-*.dll"
)
foreach ($pattern in $ffmpegDlls) {
    $files = Get-ChildItem "$VcpkgBin\$pattern" -ErrorAction SilentlyContinue
    foreach ($f in $files) {
        Copy-Item $f.FullName $DistDir
        Log "  $($f.Name)"
    }
}

# 6. Copy extra DLLs from build/Release (libx264, srt, libcrypto, etc.)
Log "Copying extra DLLs from build/Release..."
Get-ChildItem "$ProjectRoot\build\Release\*.dll" | ForEach-Object {
    Copy-Item $_.FullName $DistDir
    Log "  $($_.Name)"
}

# 7. Copy flv.min.js (required for HTTP-FLV streaming, stored alongside this script)
$flvJs = "$PSScriptRoot\flv.min.js"
if (Test-Path $flvJs) {
    Log "Copying flv.min.js..."
    Copy-Item $flvJs $DistDir
} else {
    Write-Host "[package] WARNING: flv.min.js not found at $flvJs -- HTTP-FLV player page will not work" -ForegroundColor Yellow
}

# 8. Zip
Log "Creating $ZipOut..."
if (Test-Path $ZipOut) { Remove-Item $ZipOut -Force }
Compress-Archive -Path "$DistDir\*" -DestinationPath $ZipOut

$sizeMB = [math]::Round((Get-Item $ZipOut).Length / 1MB, 1)
Log "Done! Output: $ZipOut (${sizeMB} MB)"
Log "dist\ is kept -- run dist\RambosPlayer.exe to verify before distributing"
