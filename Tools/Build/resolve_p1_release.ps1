[CmdletBinding()]
param(
    [string]$ReleaseBase = '',
    [switch]$Launch,
    [ValidateSet('sample', 'live')][string]$Provider = 'sample',
    [ValidateRange(0, 5000)][int]$PinnedLaunchDelayMilliseconds = 0
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class P1ReleaseHandleGuard {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandle(
        SafeFileHandle handle, StringBuilder path, uint length, uint flags);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct STARTUPINFO {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX;
        public int dwY;
        public int dwXSize;
        public int dwYSize;
        public int dwXCountChars;
        public int dwYCountChars;
        public int dwFillAttribute;
        public int dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFORMATION {
        public IntPtr hProcess;
        public IntPtr hThread;
        public uint dwProcessId;
        public uint dwThreadId;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateProcessW(
        string applicationName, StringBuilder commandLine,
        IntPtr processAttributes, IntPtr threadAttributes, bool inheritHandles,
        uint creationFlags, IntPtr environment, string currentDirectory,
        ref STARTUPINFO startupInfo, out PROCESS_INFORMATION processInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    public static string FinalPath(SafeFileHandle handle) {
        var buffer = new StringBuilder(32768);
        uint length = GetFinalPathNameByHandle(handle, buffer, (uint)buffer.Capacity, 0);
        if (length == 0 || length >= buffer.Capacity)
            throw new Win32Exception(Marshal.GetLastWin32Error());
        var value = buffer.ToString();
        if (value.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase))
            return @"\\" + value.Substring(8);
        if (value.StartsWith(@"\\?\", StringComparison.OrdinalIgnoreCase))
            return value.Substring(4);
        return value;
    }

    public static uint Launch(string application, string argument, string currentDirectory) {
        var startup = new STARTUPINFO();
        startup.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        var command = new StringBuilder("\"" + application + "\"");
        if (!String.IsNullOrEmpty(argument)) command.Append(" ").Append(argument);
        PROCESS_INFORMATION process;
        if (!CreateProcessW(application, command, IntPtr.Zero, IntPtr.Zero, false,
                0, IntPtr.Zero, currentDirectory, ref startup, out process))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            return process.dwProcessId;
        } finally {
            if (process.hThread != IntPtr.Zero) CloseHandle(process.hThread);
            if (process.hProcess != IntPtr.Zero) CloseHandle(process.hProcess);
        }
    }
}
'@

function Assert-NoReparseChain {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($full)
    $current = $root.TrimEnd('\','/')
    if ([string]::IsNullOrEmpty($current)) { $current = $root }
    $tail = $full.Substring($root.Length)
    $parts = @($tail.Split(@('\','/'), [StringSplitOptions]::RemoveEmptyEntries))
    $targets = @($root)
    foreach ($part in $parts) {
        $current = Join-Path $current $part
        $targets += $current
    }
    foreach ($target in $targets) {
        $item = Get-Item -LiteralPath $target -Force -ErrorAction Stop
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Release path contains a reparse point: $target"
        }
    }
}

function Open-PinnedReleaseFile {
    param([Parameter(Mandatory = $true)][string]$Path)
    Assert-NoReparseChain -Path $Path
    $expected = [IO.Path]::GetFullPath($Path).TrimEnd('\','/')
    $stream = [IO.File]::Open($expected, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        $final = [P1ReleaseHandleGuard]::FinalPath($stream.SafeFileHandle).TrimEnd('\','/')
        if (-not $final.Equals($expected, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Release file handle resolved to a different path: $Path"
        }
        return $stream
    } catch {
        $stream.Dispose()
        throw
    }
}

function Get-PinnedFileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    $stream = Open-PinnedReleaseFile -Path $Path
    try {
        return Get-StreamSha256 -Stream $stream
    } finally {
        $stream.Dispose()
    }
}

if ([string]::IsNullOrWhiteSpace($ReleaseBase)) {
    $repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
    $ReleaseBase = Join-Path $repoRoot 'release'
}
$releaseRoot = [IO.Path]::GetFullPath($ReleaseBase)
$null = Get-Item -LiteralPath $releaseRoot -Force -ErrorAction Stop
Assert-NoReparseChain -Path $releaseRoot
$latestPath = Join-Path $releaseRoot 'MountainPlanner-P1-LATEST.json'
if (-not (Test-Path -LiteralPath $latestPath -PathType Leaf)) {
    throw "No validated P1 release handoff exists: $latestPath"
}
Assert-NoReparseChain -Path $latestPath

function Get-NormalizedReleasePath {
    param([Parameter(Mandatory = $true)][string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path.IndexOf([char]0) -ge 0) {
        throw 'A release entry has an empty or invalid path.'
    }
    $value = $Path.Replace('\', '/')
    if ($value.StartsWith('/') -or $value -match '^[A-Za-z]:') {
        throw "A release entry uses an absolute path: $Path"
    }
    $parts = @($value.Split('/'))
    if ($parts.Count -eq 0 -or @($parts | Where-Object {
        [string]::IsNullOrEmpty($_) -or $_ -eq '.' -or $_ -eq '..'
    }).Count -ne 0) {
        throw "A release entry uses a non-canonical or traversal path: $Path"
    }
    return ($parts -join '/')
}

function Get-StreamSha256 {
    param([Parameter(Mandatory = $true)][IO.Stream]$Stream)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($hasher.ComputeHash($Stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        $hasher.Dispose()
    }
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
Assert-NoReparseChain -Path $artifactRoot
$launcher = Join-Path $artifactRoot 'SkiAreaDesignChallenge.exe'
$zipPath = Join-Path $releaseRoot "$artifact.zip"
$zipHashPath = "$zipPath.sha256"
$evidenceLedgerPath = Join-Path $artifactRoot 'P1-EVIDENCE-LEDGER.json'
$releaseManifestPath = Join-Path $artifactRoot 'ReleaseManifest.json'
if (Test-Path -LiteralPath (Join-Path $artifactRoot 'DO NOT USE - SOURCE CHANGED.txt') -PathType Leaf) {
    throw 'The selected P1 release is explicitly invalid.'
}
if (-not (Test-Path -LiteralPath $launcher -PathType Leaf) -or
    -not (Test-Path -LiteralPath $zipPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $zipHashPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $evidenceLedgerPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $releaseManifestPath -PathType Leaf) -or
    (Test-Path -LiteralPath "$zipPath.INVALID" -PathType Leaf)) {
    throw 'The selected P1 release is incomplete or invalidated.'
}
$pinnedLauncher = Open-PinnedReleaseFile -Path $launcher
$launcherHash = Get-StreamSha256 -Stream $pinnedLauncher
if ($launcherHash -ne ([string]$latest.LauncherSha256).ToLowerInvariant()) {
    throw 'The selected P1 launcher no longer matches its validated handoff.'
}
$zipHash = Get-PinnedFileSha256 -Path $zipPath
if ([string]$latest.ZipSha256 -notmatch '^[0-9a-f]{64}$' -or
    $zipHash -ne ([string]$latest.ZipSha256).ToLowerInvariant()) {
    throw 'The selected P1 ZIP no longer matches its validated handoff.'
}
$zipSidecar = (Get-Content -LiteralPath $zipHashPath -Raw).Trim()
if ($zipSidecar -ne "$zipHash  $artifact.zip") {
    throw 'The selected P1 ZIP checksum sidecar is missing or inconsistent.'
}
$releaseManifestHash = Get-PinnedFileSha256 -Path $releaseManifestPath
if ([string]$latest.ReleaseManifestSha256 -notmatch '^[0-9a-f]{64}$' -or
    $releaseManifestHash -ne ([string]$latest.ReleaseManifestSha256).ToLowerInvariant()) {
    throw 'The selected P1 release manifest no longer matches its validated handoff.'
}
$evidenceLedgerHash = Get-PinnedFileSha256 -Path $evidenceLedgerPath
if ([string]$latest.EvidenceLedgerSha256 -notmatch '^[0-9a-f]{64}$' -or
    $evidenceLedgerHash -ne ([string]$latest.EvidenceLedgerSha256).ToLowerInvariant()) {
    throw 'The selected P1 evidence ledger no longer matches its validated handoff.'
}

$manifest = ConvertFrom-Json (Get-Content -LiteralPath $releaseManifestPath -Raw)
if ($manifest.schemaVersion -ne 1 -or $manifest.artifact -ne $artifact -or
    $manifest.sourceDigest -ne $sourceDigest) {
    throw 'The selected P1 release manifest has the wrong identity or schema.'
}
$manifestFiles = @{}
foreach ($record in @($manifest.files)) {
    $rawPath = [string]$record.path
    $relative = Get-NormalizedReleasePath -Path $rawPath
    [long]$recordedBytes = 0
    if ($relative -ne $rawPath -or $relative -eq 'ReleaseManifest.json' -or
        $manifestFiles.ContainsKey($relative) -or
        -not [long]::TryParse([string]$record.bytes, [ref]$recordedBytes) -or
        $recordedBytes -lt 0 -or [string]$record.sha256 -notmatch '^[0-9a-f]{64}$') {
        throw "The selected P1 release manifest contains an invalid or duplicate record: $rawPath"
    }
    $manifestFiles[$relative] = @{
        Bytes = $recordedBytes
        Sha256 = ([string]$record.sha256).ToLowerInvariant()
    }
}
if ($manifestFiles.Count -eq 0) {
    throw 'The selected P1 release manifest is empty.'
}

$folderFiles = @{}
foreach ($entry in Get-ChildItem -LiteralPath $artifactRoot -Recurse -Force) {
    if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "The selected P1 release folder contains a reparse point: $($entry.FullName)"
    }
}
foreach ($file in Get-ChildItem -LiteralPath $artifactRoot -Recurse -File) {
    if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "The selected P1 release folder contains a reparse point: $($file.FullName)"
    }
    $rawRelative = $file.FullName.Substring($artifactRoot.Length).TrimStart('\','/').Replace('\','/')
    $relative = Get-NormalizedReleasePath -Path $rawRelative
    if ($folderFiles.ContainsKey($relative)) {
        throw "The selected P1 release folder contains a duplicate normalized path: $relative"
    }
    $folderFiles[$relative] = @{
        Bytes = [long]$file.Length
        Sha256 = Get-PinnedFileSha256 -Path $file.FullName
    }
}
if ($folderFiles.Count -ne ($manifestFiles.Count + 1) -or
    -not $folderFiles.ContainsKey('ReleaseManifest.json')) {
    throw 'The selected P1 release folder file set differs from ReleaseManifest.json.'
}
foreach ($relative in $manifestFiles.Keys) {
    if (-not $folderFiles.ContainsKey($relative) -or
        $folderFiles[$relative].Bytes -ne $manifestFiles[$relative].Bytes -or
        $folderFiles[$relative].Sha256 -ne $manifestFiles[$relative].Sha256) {
        throw "The selected P1 release folder differs from its manifest: $relative"
    }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $archiveFiles = @{}
    $fileEntries = @()
    foreach ($entry in $archive.Entries) {
        $raw = [string]$entry.FullName
        $isDirectory = [string]::IsNullOrEmpty($entry.Name) -or $raw.EndsWith('/') -or $raw.EndsWith('\')
        $pathForNormalization = if ($isDirectory) { $raw.TrimEnd('/','\') } else { $raw }
        if ([string]::IsNullOrEmpty($pathForNormalization)) {
            throw 'The selected P1 ZIP contains an invalid root entry.'
        }
        $normalized = Get-NormalizedReleasePath -Path $pathForNormalization
        $external = [uint32]([int64]$entry.ExternalAttributes -band 0xffffffffL)
        $unixType = (($external -shr 16) -band 0xf000)
        if ($isDirectory) {
            if ($entry.Length -ne 0 -or ($unixType -ne 0 -and $unixType -ne 0x4000)) {
                throw "The selected P1 ZIP contains a non-directory directory entry: $raw"
            }
            continue
        }
        if ($unixType -ne 0 -and $unixType -ne 0x8000) {
            throw "The selected P1 ZIP contains a non-regular file entry: $raw"
        }
        $fileEntries += [pscustomobject]@{ Entry = $entry; Path = $normalized }
    }
    if ($fileEntries.Count -eq 0) {
        throw 'The selected P1 ZIP contains no regular files.'
    }
    $artifactPrefix = "$artifact/"
    $prefixedCount = @($fileEntries | Where-Object {
        $_.Path.StartsWith($artifactPrefix, [StringComparison]::Ordinal)
    }).Count
    if ($prefixedCount -ne 0 -and $prefixedCount -ne $fileEntries.Count) {
        throw 'The selected P1 ZIP mixes release-root-prefixed and unprefixed entries.'
    }
    foreach ($item in $fileEntries) {
        $relative = if ($prefixedCount -eq $fileEntries.Count) {
            $item.Path.Substring($artifactPrefix.Length)
        } else {
            $item.Path
        }
        if ([string]::IsNullOrEmpty($relative) -or $archiveFiles.ContainsKey($relative)) {
            throw "The selected P1 ZIP contains duplicate normalized entries: $relative"
        }
        $archiveFiles[$relative] = $item.Entry
    }
    if ($archiveFiles.Count -ne $folderFiles.Count) {
        throw 'The selected P1 ZIP regular-file set differs from the release folder.'
    }
    foreach ($relative in $folderFiles.Keys) {
        if (-not $archiveFiles.ContainsKey($relative)) {
            throw "The selected P1 ZIP is missing a release file: $relative"
        }
        $entry = $archiveFiles[$relative]
        if ([long]$entry.Length -ne $folderFiles[$relative].Bytes) {
            throw "The selected P1 ZIP has the wrong file size: $relative"
        }
        $stream = $entry.Open()
        try {
            $entryHash = Get-StreamSha256 -Stream $stream
        } finally {
            $stream.Dispose()
        }
        if ($entryHash -ne $folderFiles[$relative].Sha256) {
            throw "The selected P1 ZIP has the wrong file content: $relative"
        }
    }
} finally {
    $archive.Dispose()
}

try {
    if ($Launch) {
        if ($PinnedLaunchDelayMilliseconds -gt 0) {
            Start-Sleep -Milliseconds $PinnedLaunchDelayMilliseconds
        }
        $argument = if ($Provider -eq 'sample') { '-SkiUseFixtureProvider' } else { '' }
        $processId = [P1ReleaseHandleGuard]::Launch($launcher, $argument, $artifactRoot)
        Write-Output "LAUNCHED:$processId"
    } else {
        Write-Output $launcher
    }
} finally {
    $pinnedLauncher.Dispose()
}
