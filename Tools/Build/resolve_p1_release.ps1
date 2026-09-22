[CmdletBinding()]
param(
    [string]$ReleaseBase = ''
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($ReleaseBase)) {
    $repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
    $ReleaseBase = Join-Path $repoRoot 'release'
}
$releaseRoot = [IO.Path]::GetFullPath($ReleaseBase)
$latestPath = Join-Path $releaseRoot 'MountainPlanner-P1-LATEST.json'
if (-not (Test-Path -LiteralPath $latestPath -PathType Leaf)) {
    throw "No validated P1 release handoff exists: $latestPath"
}

$latest = ConvertFrom-Json (Get-Content -LiteralPath $latestPath -Raw)
$artifact = [string]$latest.Artifact
$sourceDigest = [string]$latest.SourceDigest
if ($latest.Status -ne 'PASS' -or $sourceDigest -notmatch '^[0-9a-f]{64}$' -or
    $artifact -notmatch '^MountainPlanner-P1-Windows-[0-9a-f]{8}-[0-9]{8}T[0-9]{6}$' -or
    -not $artifact.Contains($sourceDigest.Substring(0, 8))) {
    throw 'The P1 latest-release handoff is malformed or not passing.'
}

$artifactRoot = [IO.Path]::GetFullPath((Join-Path $releaseRoot $artifact))
$requiredPrefix = $releaseRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
if (-not $artifactRoot.StartsWith($requiredPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The P1 release handoff resolves outside the release directory.'
}
$launcher = Join-Path $artifactRoot 'SkiAreaDesignChallenge.exe'
$zipPath = Join-Path $releaseRoot "$artifact.zip"
if (Test-Path -LiteralPath (Join-Path $artifactRoot 'DO NOT USE - SOURCE CHANGED.txt') -PathType Leaf) {
    throw 'The selected P1 release is explicitly invalid.'
}
if (-not (Test-Path -LiteralPath $launcher -PathType Leaf) -or
    -not (Test-Path -LiteralPath $zipPath -PathType Leaf) -or
    (Test-Path -LiteralPath "$zipPath.INVALID" -PathType Leaf)) {
    throw 'The selected P1 release is incomplete or invalidated.'
}
$launcherHash = (Get-FileHash -LiteralPath $launcher -Algorithm SHA256).Hash.ToLowerInvariant()
if ($launcherHash -ne ([string]$latest.LauncherSha256).ToLowerInvariant()) {
    throw 'The selected P1 launcher no longer matches its validated handoff.'
}

Write-Output $launcher
