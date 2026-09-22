[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$releaseBase = [IO.Path]::GetFullPath((Join-Path $repoRoot 'release'))
$freezePath = Join-Path $repoRoot 'test-results\p1\release-freeze.json'
$freezeCheckPath = Join-Path $repoRoot 'test-results\p1\freeze-check.json'
$receiptPath = Join-Path $repoRoot 'test-results\p1\package-Shipping.json'
$tiffEditorReceiptPath = Join-Path $repoRoot 'test-results\p1\tiff.json'
$acquisitionEditorReceiptPath = Join-Path $repoRoot 'test-results\p1\acquisition.json'
$uiEditorReceiptPath = Join-Path $repoRoot 'test-results\p1\ui.json'
$terrainCoreEditorReceiptPath = Join-Path $repoRoot 'test-results\p1\terraincore.json'
$automationReceiptPath = Join-Path $repoRoot 'test-results\p1\automation.json'
$tiffReceiptPath = Join-Path $repoRoot 'test-results\p1\smoke-Shipping-geotiff-regression.json'
$acquisitionReceiptPath = Join-Path $repoRoot 'test-results\p1\smoke-Shipping-acquisition-regression.json'
$uiReceiptPath = Join-Path $repoRoot 'test-results\p1\smoke-Shipping-ui-layout.json'
$terrainCoreReceiptPath = Join-Path $repoRoot 'test-results\p1\smoke-Shipping-terraincore-regression.json'

if (-not (Test-Path -LiteralPath $freezePath -PathType Leaf)) {
    throw "Release source freeze is missing: $freezePath"
}
$env:SKI_P1_RELEASE_FREEZE = $freezePath
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    & $python.Source (Join-Path $repoRoot 'Tools\Build\p1.py') freeze-check
} else {
    & py -3 (Join-Path $repoRoot 'Tools\Build\p1.py') freeze-check
}
if ($LASTEXITCODE -ne 0) {
    throw 'Current source no longer matches the release-wide freeze receipt.'
}

$freeze = ConvertFrom-Json (Get-Content -LiteralPath $freezePath -Raw)
$freezeCheck = ConvertFrom-Json (Get-Content -LiteralPath $freezeCheckPath -Raw)
if ($freeze.command -ne 'freeze' -or $freeze.result.status -ne 'PASS' -or
    $freezeCheck.command -ne 'freeze-check' -or $freezeCheck.result.status -ne 'PASS' -or
    $freezeCheck.source_before -ne $freeze.source_before) {
    throw 'Release source freeze receipts are missing, failed, or inconsistent.'
}

if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
    throw "Shipping package receipt is missing: $receiptPath"
}

$receipt = ConvertFrom-Json (Get-Content -LiteralPath $receiptPath -Raw)
if ($receipt.result.status -ne 'PASS' -or $receipt.configuration -ne 'Shipping') {
    throw 'The latest Shipping package receipt is not a PASS.'
}
if ($receipt.source_before -ne $freeze.source_before) {
    throw 'The Shipping package was not built from the release-wide frozen source.'
}

$sourceDigest = [string]$freeze.source_before
$packageInvocation = [string]$receipt.invocation
if ($sourceDigest -notmatch '^[0-9a-f]{64}$' -or
    $packageInvocation -notmatch '^(?<timestamp>[0-9]{8}T[0-9]{6})\.') {
    throw 'Release identity is malformed; refusing to derive output paths.'
}
$artifactName = "MountainPlanner-P1-Windows-$($sourceDigest.Substring(0, 8))-$($Matches.timestamp)"
$releaseRoot = [IO.Path]::GetFullPath((Join-Path $releaseBase $artifactName))
$zipPath = [IO.Path]::GetFullPath((Join-Path $releaseBase "$artifactName.zip"))
$hashPath = "$zipPath.sha256"
if (-not $releaseRoot.StartsWith($releaseBase + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    -not $zipPath.StartsWith($releaseBase + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to assemble outside the repository release directory: $releaseRoot"
}

function Assert-EditorGateReceipt {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Command
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Editor gate receipt is missing: $Path"
    }
    $gate = ConvertFrom-Json (Get-Content -LiteralPath $Path -Raw)
    if ($gate.command -ne $Command -or $gate.result.status -ne 'PASS' -or
        $gate.source_before -ne $freeze.source_before) {
        throw "Editor gate '$Command' is failed, stale, or belongs to another source."
    }
    return $gate
}

$tiffEditorReceipt = Assert-EditorGateReceipt -Path $tiffEditorReceiptPath -Command 'tiff'
$acquisitionEditorReceipt = Assert-EditorGateReceipt -Path $acquisitionEditorReceiptPath -Command 'acquisition'
$uiEditorReceipt = Assert-EditorGateReceipt -Path $uiEditorReceiptPath -Command 'ui'
$terrainCoreEditorReceipt = Assert-EditorGateReceipt -Path $terrainCoreEditorReceiptPath -Command 'terraincore'
$automationReceipt = Assert-EditorGateReceipt -Path $automationReceiptPath -Command 'automation'
$expectedNativeTerrainCoreTests = @('SkiDomain.Revision', 'SkiDomain.Terrain', 'SkiDomain.TerrainCore')
$recordedNativeTests = @($terrainCoreEditorReceipt.result.native_receipts | ForEach-Object { [string]$_.name } | Sort-Object)
if (($recordedNativeTests -join '|') -ne (($expectedNativeTerrainCoreTests | Sort-Object) -join '|') -or
    @($terrainCoreEditorReceipt.result.native_receipts | Where-Object {
        $_.result -ne 'PASS' -or [string]$_.executable_sha256 -notmatch '^[0-9a-f]{64}$'
    }).Count -ne 0) {
    throw 'The exact native TerrainCore CTest identities are not bound to passing executable receipts.'
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

function Assert-ShippingRegressionReceipt {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Scenario,
        [Parameter(Mandatory = $true)][string]$Label
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Shipping $Label receipt is missing: $Path"
    }
    $scenarioReceipt = ConvertFrom-Json (Get-Content -LiteralPath $Path -Raw)
    if ($scenarioReceipt.result.status -ne 'PASS' -or
        $scenarioReceipt.result.scenario -ne $Scenario -or
        $scenarioReceipt.configuration -ne 'Shipping' -or
        $scenarioReceipt.source_before -ne $receipt.source_before -or
        $scenarioReceipt.result.package_invocation -ne $receipt.invocation) {
        throw "The Shipping $Label regression is absent, stale, or belongs to another package."
    }
    return $scenarioReceipt
}

$acquisitionReceipt = Assert-ShippingRegressionReceipt -Path $acquisitionReceiptPath -Scenario 'acquisition-regression' -Label 'acquisition-policy'
$uiReceipt = Assert-ShippingRegressionReceipt -Path $uiReceiptPath -Scenario 'ui-layout' -Label 'UI-layout'
$terrainCoreReceipt = Assert-ShippingRegressionReceipt -Path $terrainCoreReceiptPath -Scenario 'terraincore-regression' -Label 'TerrainCore'
$uiProcessAudit = $uiReceipt.result.process_network_audit
if ($uiProcessAudit.method -ne 'GetExtendedTcpTable process-attributed polling' -or
    $uiProcessAudit.snapshots -lt 1 -or @($uiProcessAudit.listen_ports).Count -ne 0) {
    throw 'The Shipping UI scenario lacks a process-attributed no-listener audit.'
}
$terrainCoreProof = $terrainCoreReceipt.result.receipt
if ($terrainCoreProof.schemaVersion -ne 2 -or
    [string]$terrainCoreProof.contentId -notmatch '^[0-9a-f]{64}$' -or
    [string]$terrainCoreProof.editSetId -notmatch '^[0-9a-f]{64}$' -or
    -not $terrainCoreProof.baseImmutable -or -not $terrainCoreProof.offlineReopen -or
    -not $terrainCoreProof.deterministicIds -or -not $terrainCoreProof.deterministicTrees -or
    -not $terrainCoreProof.rendererPath -or $terrainCoreProof.renderedTileCount -le 0 -or
    -not $terrainCoreProof.revisionAligned -or -not $terrainCoreProof.actorPicked -or
    -not $terrainCoreProof.actorMutationObserved -or
    -not $terrainCoreProof.actorStaleMeshRejected -or
    -not $terrainCoreProof.acquisitionPortGuardInstalled -or
    $terrainCoreProof.acquisitionTransportCalls -ne 0 -or
    [string]$terrainCoreProof.contractTrees.TerrainCore.sha256 -notmatch '^[0-9a-f]{64}$' -or
    [string]$terrainCoreProof.contractTrees.TerrainEdits.sha256 -notmatch '^[0-9a-f]{64}$' -or
    $terrainCoreProof.contractTrees.TerrainCore.sha256 -ne $terrainCoreProof.contractTreeRuns.first.TerrainCore.sha256 -or
    $terrainCoreProof.contractTrees.TerrainEdits.sha256 -ne $terrainCoreProof.contractTreeRuns.first.TerrainEdits.sha256 -or
    $terrainCoreProof.contractTreeRuns.first.TerrainCore.sha256 -ne $terrainCoreProof.contractTreeRuns.repeat.TerrainCore.sha256 -or
    $terrainCoreProof.contractTreeRuns.first.TerrainCore.sha256 -ne $terrainCoreProof.contractTreeRuns.reopen.TerrainCore.sha256 -or
    $terrainCoreProof.contractTreeRuns.first.TerrainEdits.sha256 -ne $terrainCoreProof.contractTreeRuns.repeat.TerrainEdits.sha256 -or
    $terrainCoreProof.contractTreeRuns.first.TerrainEdits.sha256 -ne $terrainCoreProof.contractTreeRuns.reopen.TerrainEdits.sha256 -or
    $terrainCoreProof.defaultCacheBudgetBytes -ne 536870912 -or
    $terrainCoreProof.processNetworkAudit.method -ne 'GetExtendedTcpTable process-attributed polling' -or
    $terrainCoreProof.processNetworkAudit.snapshots -lt 1 -or
    @($terrainCoreProof.processNetworkAudit.listen_ports).Count -ne 0 -or
    @($terrainCoreProof.processNetworkAudit.remote_endpoints).Count -ne 0) {
    throw 'The Shipping TerrainCore receipt is incomplete or does not prove immutable offline reopen.'
}
$terrainCoreChildren = @($terrainCoreProof.children)
if ($terrainCoreChildren.Count -ne 3 -or @($terrainCoreChildren | Where-Object {
    -not $_.rendererPath -or $_.renderedTileCount -le 0 -or
    -not $_.revisionAligned -or -not $_.actorPicked -or -not $_.actorMutationObserved -or
    -not $_.actorStaleMeshRejected
}).Count -ne 0) {
    throw 'Every Shipping TerrainCore child must prove rendering, picking, and revision alignment.'
}
$offlineChild = @($terrainCoreChildren | Where-Object { $_.scenario -eq 'terraincore-offline-reopen' })
if ($offlineChild.Count -ne 1 -or -not $offlineChild[0].acquisitionPortGuardInstalled -or
    $offlineChild[0].acquisitionTransportCalls -ne 0) {
    throw 'The offline child lacks production acquisition-port denial.'
}

$shippingMcpProof = $receipt.result.shipping_mcp_proof
if ($shippingMcpProof.status -ne 'PASS' -or
    $shippingMcpProof.target -ne 'SkiAreaDesignChallenge' -or
    $shippingMcpProof.platform -ne 'Win64' -or
    $shippingMcpProof.configuration -ne 'Shipping' -or
    $shippingMcpProof.target_type -ne 'Game' -or
    $shippingMcpProof.root_module -ne 'SkiPresentation' -or
    [string]$shippingMcpProof.target_receipt_sha256 -notmatch '^[0-9a-f]{64}$' -or
    [string]$shippingMcpProof.target_rules_sha256 -notmatch '^[0-9a-f]{64}$' -or
    @($shippingMcpProof.forbidden_plugins_absent).Count -ne 5) {
    throw 'The Shipping package lacks exact Game-target/module proof for MCP exclusion.'
}

$requiredEvidenceReceipts = [ordered]@{
    'editor-tiff' = @{ Path = $tiffEditorReceiptPath; Receipt = $tiffEditorReceipt }
    'editor-acquisition' = @{ Path = $acquisitionEditorReceiptPath; Receipt = $acquisitionEditorReceipt }
    'editor-ui' = @{ Path = $uiEditorReceiptPath; Receipt = $uiEditorReceipt }
    'editor-terraincore' = @{ Path = $terrainCoreEditorReceiptPath; Receipt = $terrainCoreEditorReceipt }
    'editor-automation' = @{ Path = $automationReceiptPath; Receipt = $automationReceipt }
    'shipping-package' = @{ Path = $receiptPath; Receipt = $receipt }
    'shipping-geotiff' = @{ Path = $tiffReceiptPath; Receipt = $tiffReceipt }
    'shipping-acquisition' = @{ Path = $acquisitionReceiptPath; Receipt = $acquisitionReceipt }
    'shipping-ui' = @{ Path = $uiReceiptPath; Receipt = $uiReceipt }
    'shipping-terraincore' = @{ Path = $terrainCoreReceiptPath; Receipt = $terrainCoreReceipt }
}
$runsRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'test-results\p1\runs'))

$packageRoot = [IO.Path]::GetFullPath([string]$receipt.result.directory)
$packageWindows = Join-Path $packageRoot 'Windows'
$packageExe = Join-Path $packageWindows 'SkiAreaDesignChallenge.exe'
if (-not (Test-Path -LiteralPath $packageExe -PathType Leaf)) {
    throw "The packaged executable is missing: $packageExe"
}

function Assert-RecordedPackageFiles {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)]$Manifest,
        [string]$StripPrefix = ''
    )
    $expected = @{}
    foreach ($property in $Manifest.files.PSObject.Properties) {
        $relative = [string]$property.Name
        if ($StripPrefix) {
            if (-not $relative.StartsWith($StripPrefix, [StringComparison]::Ordinal)) {
                throw "Recorded package path is outside expected prefix '$StripPrefix': $relative"
            }
            $relative = $relative.Substring($StripPrefix.Length)
        }
        $expected[$relative] = [string]$property.Value
    }
    $actual = @{}
    foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File) {
        $relative = $file.FullName.Substring(([IO.Path]::GetFullPath($Root)).Length).TrimStart('\','/').Replace('\','/')
        $actual[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    if ($actual.Count -ne $expected.Count) {
        throw "Packaged file count changed: expected $($expected.Count), found $($actual.Count)."
    }
    foreach ($relative in $expected.Keys) {
        if (-not $actual.ContainsKey($relative) -or $actual[$relative] -ne $expected[$relative]) {
            throw "Packaged file is missing or changed: $relative"
        }
    }
}

Assert-RecordedPackageFiles -Root $packageRoot -Manifest $receipt.result.manifest
if ((Get-FileHash -LiteralPath $packageExe -Algorithm SHA256).Hash.ToLowerInvariant() -ne
    ([string]$receipt.result.launcher_sha256).ToLowerInvariant()) {
    throw 'The packaged executable hash differs from the package receipt.'
}

New-Item -ItemType Directory -Path $releaseBase -Force | Out-Null
if (Test-Path -LiteralPath $releaseRoot) {
    Remove-Item -LiteralPath $releaseRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $releaseRoot | Out-Null
Copy-Item -Path (Join-Path $packageWindows '*') -Destination $releaseRoot -Recurse -Force
Assert-RecordedPackageFiles -Root $releaseRoot -Manifest $receipt.result.manifest -StripPrefix 'Windows/'

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
set "SAVED=%LOCALAPPDATA%\SkiAreaDesignChallenge\Saved"
if not exist "%SAVED%\TerrainDiagnostics" mkdir "%SAVED%\TerrainDiagnostics"
start "Mountain Planner Diagnostics" "%SAVED%"
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
bounded, redacted preparation logs and failure receipts. Unreal Logs and Crashes
also appear there when available. Raw downloaded terrain payloads are not kept.

This is a P1 terrain-preparation test build. Construction and simulation gameplay
are outside this build's scope.
'@
Set-Content -LiteralPath (Join-Path $releaseRoot 'README FIRST.txt') -Value $readme -Encoding UTF8

$evidenceLedgerPath = Join-Path $releaseRoot 'P1-EVIDENCE-LEDGER.json'
$evidenceLedgerArguments = @(
    (Join-Path $repoRoot 'Tools\Build\evidence_ledger.py'),
    '--repo-root', $repoRoot,
    '--runs-root', $runsRoot,
    '--source-digest', $sourceDigest,
    '--package-invocation', $packageInvocation,
    '--output', $evidenceLedgerPath
)
foreach ($label in $requiredEvidenceReceipts.Keys) {
    $evidenceLedgerArguments += @('--receipt', "$label=$($requiredEvidenceReceipts[$label].Path)")
}
$evidenceLedgerArguments += @(
    '--require', 'editor-terraincore=terraincore-ctest-discovery.json',
    '--require', 'editor-terraincore=terraincore-ctest-results.xml',
    '--require', 'editor-terraincore=terraincore-ctest-run.log'
)
if ($python) {
    & $python.Source @evidenceLedgerArguments
} else {
    & py -3 @evidenceLedgerArguments
}
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $evidenceLedgerPath -PathType Leaf)) {
    throw 'The release evidence ledger could not be validated and assembled.'
}
$evidenceLedgerHash = (Get-FileHash -LiteralPath $evidenceLedgerPath -Algorithm SHA256).Hash.ToLowerInvariant()

$manifestHash = [string]$receipt.result.manifest.sha256
$buildInfo = @"
Mountain Planner Unreal P1
Package invocation: $($receipt.invocation)
Source digest: $($receipt.source_before)
Package manifest SHA-256: $manifestHash
Editor GeoTIFF gate: $($tiffEditorReceipt.invocation)
Editor acquisition gate: $($acquisitionEditorReceipt.invocation)
Editor UI gate: $($uiEditorReceipt.invocation)
Editor TerrainCore gate: $($terrainCoreEditorReceipt.invocation)
Full P1 automation gate: $($automationReceipt.invocation)
Shipping GeoTIFF regression: $($tiffReceipt.invocation)
Shipping acquisition regression: $($acquisitionReceipt.invocation)
Shipping UI-layout regression: $($uiReceipt.invocation)
Shipping TerrainCore regression: $($terrainCoreReceipt.invocation)
Shipping target receipt SHA-256: $($shippingMcpProof.target_receipt_sha256)
Shipping process TCP audit snapshots: $($terrainCoreProof.processNetworkAudit.snapshots)
TerrainCore default cache bytes: $($terrainCoreProof.defaultCacheBudgetBytes)
Evidence ledger SHA-256: $evidenceLedgerHash
Assembled UTC: $([DateTime]::UtcNow.ToString('o'))
"@
Set-Content -LiteralPath (Join-Path $releaseRoot 'BUILD-INFO.txt') -Value $buildInfo -Encoding Ascii

$releaseManifestPath = Join-Path $releaseRoot 'ReleaseManifest.json'
$releaseManifestFiles = @(
    foreach ($file in Get-ChildItem -LiteralPath $releaseRoot -Recurse -File | Sort-Object FullName) {
        [ordered]@{
            path = $file.FullName.Substring($releaseRoot.Length).TrimStart('\','/').Replace('\','/')
            bytes = [long]$file.Length
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
)
[ordered]@{
    schemaVersion = 1
    artifact = $artifactName
    sourceDigest = $sourceDigest
    files = $releaseManifestFiles
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $releaseManifestPath -Encoding UTF8
$releaseManifestHash = (Get-FileHash -LiteralPath $releaseManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
if (Test-Path -LiteralPath $hashPath) {
    Remove-Item -LiteralPath $hashPath -Force
}

Compress-Archive -LiteralPath $releaseRoot -DestinationPath $zipPath -CompressionLevel Optimal
$zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath $hashPath -Value "$zipHash  $artifactName.zip" -Encoding Ascii

# Close the copy/compression race: a source change at any point during assembly
# invalidates the output even when all pre-assembly receipts matched.
if ($python) {
    & $python.Source (Join-Path $repoRoot 'Tools\Build\p1.py') freeze-check
} else {
    & py -3 (Join-Path $repoRoot 'Tools\Build\p1.py') freeze-check
}
if ($LASTEXITCODE -ne 0) {
    Set-Content -LiteralPath (Join-Path $releaseRoot 'DO NOT USE - SOURCE CHANGED.txt') `
        -Value 'Source changed during release assembly. Rebuild from a new freeze.' -Encoding Ascii
    if (Test-Path -LiteralPath $zipPath) {
        Move-Item -LiteralPath $zipPath -Destination ($zipPath + '.INVALID') -Force
    }
    if (Test-Path -LiteralPath $hashPath) {
        Move-Item -LiteralPath $hashPath -Destination ($hashPath + '.INVALID') -Force
    }
    throw 'Source changed during release assembly; output was marked invalid.'
}
$finalFreezeCheck = ConvertFrom-Json (Get-Content -LiteralPath $freezeCheckPath -Raw)
if ($finalFreezeCheck.command -ne 'freeze-check' -or
    $finalFreezeCheck.result.status -ne 'PASS' -or
    $finalFreezeCheck.source_before -ne $freeze.source_before) {
    throw 'Final release source-freeze receipt is missing, failed, or inconsistent.'
}

$fileCount = (Get-ChildItem -LiteralPath $releaseRoot -Recurse -File).Count
$folderBytes = (Get-ChildItem -LiteralPath $releaseRoot -Recurse -File | Measure-Object -Property Length -Sum).Sum
$zipBytes = (Get-Item -LiteralPath $zipPath).Length
$latestPath = Join-Path $releaseBase 'MountainPlanner-P1-LATEST.json'
$latestTemporaryPath = "$latestPath.$PID.tmp"
$launcherHash = (Get-FileHash -LiteralPath (Join-Path $releaseRoot 'SkiAreaDesignChallenge.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
[pscustomobject]@{
    Status = 'PASS'
    Artifact = $artifactName
    SourceDigest = $sourceDigest
    PackageInvocation = $packageInvocation
    LauncherSha256 = $launcherHash
    ReleaseManifestSha256 = $releaseManifestHash
    EvidenceLedgerSha256 = $evidenceLedgerHash
    ZipSha256 = $zipHash
} | ConvertTo-Json | Set-Content -LiteralPath $latestTemporaryPath -Encoding UTF8
Move-Item -LiteralPath $latestTemporaryPath -Destination $latestPath -Force

[pscustomobject]@{
    Status = 'PASS'
    Folder = $releaseRoot
    Zip = $zipPath
    ZipSha256 = $zipHash
    Files = $fileCount
    FolderBytes = $folderBytes
    ZipBytes = $zipBytes
    LatestReceipt = $latestPath
} | Format-List
