[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$releaseBase = [IO.Path]::GetFullPath((Join-Path $repoRoot 'release'))
$releaseRoot = [IO.Path]::GetFullPath((Join-Path $releaseBase 'MountainPlanner-P1-Windows'))
$zipPath = [IO.Path]::GetFullPath((Join-Path $releaseBase 'MountainPlanner-P1-Windows.zip'))
$hashPath = "$zipPath.sha256"
$receiptPath = Join-Path $repoRoot 'test-results\p1\package-Shipping.json'
$tiffReceiptPath = Join-Path $repoRoot 'test-results\p1\smoke-Shipping.json'

if (-not $releaseRoot.StartsWith($releaseBase + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to assemble outside the repository release directory: $releaseRoot"
}

if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
    throw "Shipping package receipt is missing: $receiptPath"
}

$receipt = ConvertFrom-Json (Get-Content -LiteralPath $receiptPath -Raw)
if ($receipt.result.status -ne 'PASS' -or $receipt.configuration -ne 'Shipping') {
    throw 'The latest Shipping package receipt is not a PASS.'
}
if (-not (Test-Path -LiteralPath $tiffReceiptPath -PathType Leaf)) {
    throw "Shipping GeoTIFF regression receipt is missing: $tiffReceiptPath"
}
$tiffReceipt = ConvertFrom-Json (Get-Content -LiteralPath $tiffReceiptPath -Raw)
if ($tiffReceipt.result.status -ne 'PASS' -or
    $tiffReceipt.result.scenario -ne 'geotiff-regression' -or
    $tiffReceipt.configuration -ne 'Shipping' -or
    $tiffReceipt.source_before -ne $receipt.source_before -or
    $tiffReceipt.result.package_invocation -ne $receipt.invocation) {
    throw 'The Shipping GeoTIFF regression is absent, stale, or belongs to another package.'
}

$packageRoot = [IO.Path]::GetFullPath([string]$receipt.result.directory)
$packageWindows = Join-Path $packageRoot 'Windows'
$packageExe = Join-Path $packageWindows 'SkiAreaDesignChallenge.exe'
if (-not (Test-Path -LiteralPath $packageExe -PathType Leaf)) {
    throw "The packaged executable is missing: $packageExe"
}

New-Item -ItemType Directory -Path $releaseBase -Force | Out-Null
if (Test-Path -LiteralPath $releaseRoot) {
    Remove-Item -LiteralPath $releaseRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $releaseRoot | Out-Null
Copy-Item -Path (Join-Path $packageWindows '*') -Destination $releaseRoot -Recurse -Force

Get-ChildItem -LiteralPath $releaseRoot -Filter 'Manifest_*.txt' -File | Remove-Item -Force

$normalLauncher = @'
@echo off
cd /d "%~dp0"
start "Mountain Planner" "SkiAreaDesignChallenge.exe"
'@
Set-Content -LiteralPath (Join-Path $releaseRoot 'START MOUNTAIN PLANNER.bat') -Value $normalLauncher -Encoding Ascii

$sampleLauncher = @'
@echo off
cd /d "%~dp0"
start "Mountain Planner - Sample Terrain" "SkiAreaDesignChallenge.exe" -SkiUseFixtureProvider
'@
Set-Content -LiteralPath (Join-Path $releaseRoot 'START SAMPLE TERRAIN.bat') -Value $sampleLauncher -Encoding Ascii

$diagnosticsLauncher = @'
@echo off
set "DIAGNOSTICS=%LOCALAPPDATA%\SkiAreaDesignChallenge\Saved\TerrainDiagnostics"
if not exist "%DIAGNOSTICS%" mkdir "%DIAGNOSTICS%"
start "Mountain Planner Diagnostics" "%DIAGNOSTICS%"
'@
Set-Content -LiteralPath (Join-Path $releaseRoot 'OPEN DIAGNOSTICS.bat') -Value $diagnosticsLauncher -Encoding Ascii

$readme = @'
MOUNTAIN PLANNER - UNREAL P1 TEST BUILD
=======================================

QUICK START

1. Keep this entire folder together. Do not move the EXE out by itself.
2. Double-click START SAMPLE TERRAIN.bat for the reliable built-in test terrain.
3. In the selector, click the map, choose a 2-10 km size and profile, then click
   Prepare selected mountain.

LIVE TERRAIN

Double-click START MOUNTAIN PLANNER.bat to use live terrain providers. This mode
requires internet access for the selector map, USGS elevation and ESA WorldCover.
Live provider qualification is still in progress, so begin with sample terrain.

REQUIREMENTS

- 64-bit Windows 10 or Windows 11
- A DirectX-capable GPU
- Internet access for live terrain

The application is self-contained. Testers do not need Unreal Editor, Python,
Node.js, a development server or the source repository.

If preparation fails, double-click OPEN DIAGNOSTICS.bat. The folder contains
bounded, redacted preparation logs and failure receipts; it does not contain
raw downloaded terrain payloads.

This is a P1 terrain-preparation test build. Construction and simulation gameplay
are outside this build's scope.
'@
Set-Content -LiteralPath (Join-Path $releaseRoot 'README FIRST.txt') -Value $readme -Encoding UTF8

$manifestHash = [string]$receipt.result.manifest.sha256
$buildInfo = @"
Mountain Planner Unreal P1
Package invocation: $($receipt.invocation)
Source digest: $($receipt.source_before)
Package manifest SHA-256: $manifestHash
Shipping GeoTIFF regression: $($tiffReceipt.invocation)
Assembled UTC: $([DateTime]::UtcNow.ToString('o'))
"@
Set-Content -LiteralPath (Join-Path $releaseRoot 'BUILD-INFO.txt') -Value $buildInfo -Encoding Ascii

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
if (Test-Path -LiteralPath $hashPath) {
    Remove-Item -LiteralPath $hashPath -Force
}

Compress-Archive -LiteralPath $releaseRoot -DestinationPath $zipPath -CompressionLevel Optimal
$zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath $hashPath -Value "$zipHash  MountainPlanner-P1-Windows.zip" -Encoding Ascii

$fileCount = (Get-ChildItem -LiteralPath $releaseRoot -Recurse -File).Count
$folderBytes = (Get-ChildItem -LiteralPath $releaseRoot -Recurse -File | Measure-Object -Property Length -Sum).Sum
$zipBytes = (Get-Item -LiteralPath $zipPath).Length

[pscustomobject]@{
    Status = 'PASS'
    Folder = $releaseRoot
    Zip = $zipPath
    ZipSha256 = $zipHash
    Files = $fileCount
    FolderBytes = $folderBytes
    ZipBytes = $zipBytes
} | Format-List
