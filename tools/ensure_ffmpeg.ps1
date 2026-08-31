[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$PublicRoot = Split-Path -Parent $PSScriptRoot
$DependencyRoot = Join-Path $PublicRoot 'tools\.build\dependencies\ffmpeg'
$ArchiveRoot = Join-Path $PublicRoot 'tools\.build\downloads'
$AssetName = 'ffmpeg-N-126313-g1ae4048218-win64-lgpl-shared.zip'
$ReleaseBase = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-08-29-13-12'
$ChecksumsName = 'checksums.sha256'
$ChecksumsSha256 = '3D9D4AAF0D4B1A9CB28F36847680E5C585A71FE29F40E5F948D211E8735BE056'

function Find-FfmpegRoot([string]$SearchRoot) {
    $Header = Get-ChildItem -LiteralPath $SearchRoot -Recurse -File -Filter avcodec.h -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '[\\/]include[\\/]libavcodec[\\/]avcodec[.]h$' } |
        Select-Object -First 1
    if (-not $Header) { return $null }
    $Root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $Header.FullName))
    foreach ($Required in @('include\libavcodec\avcodec.h','lib\avcodec.lib','bin')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $Required))) { return $null }
    }
    return $Root
}

$Existing = Find-FfmpegRoot $DependencyRoot
if ($Existing) {
    Write-Output $Existing
    exit 0
}

New-Item -ItemType Directory -Force -Path $ArchiveRoot | Out-Null
$ChecksumsPath = Join-Path $ArchiveRoot $ChecksumsName
$ArchivePath = Join-Path $ArchiveRoot $AssetName
$TemporaryChecksumsPath = "$ChecksumsPath.$PID.partial"
$TemporaryArchivePath = "$ArchivePath.$PID.partial"

function Test-FileSha256([string]$Path, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    try { return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash -eq $Expected }
    catch [System.IO.IOException] { return $false }
}

Write-Host 'Downloading the pinned FFmpeg LGPL checksum manifest...'
try {
    Invoke-WebRequest -UseBasicParsing -Uri "$ReleaseBase/$ChecksumsName" -OutFile $TemporaryChecksumsPath
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $TemporaryChecksumsPath).Hash -ne $ChecksumsSha256) {
        throw 'The FFmpeg checksum manifest failed its pinned SHA-256 verification.'
    }
    $ChecksumLine = Get-Content -LiteralPath $TemporaryChecksumsPath | Where-Object { $_ -match "[ *]$([regex]::Escape($AssetName))$" } | Select-Object -First 1
} finally {
    Remove-Item -LiteralPath $TemporaryChecksumsPath -Force -ErrorAction SilentlyContinue
}
if (-not $ChecksumLine -or $ChecksumLine -notmatch '^([0-9a-fA-F]{64})') {
    throw "The pinned FFmpeg asset is absent from $ChecksumsName."
}
$ExpectedArchiveSha256 = $Matches[1].ToUpperInvariant()

if (Test-FileSha256 $ArchivePath $ExpectedArchiveSha256) {
    $ArchiveToExtract = $ArchivePath
} else {
    Write-Host 'Downloading the pinned Windows x64 FFmpeg LGPL shared development package...'
    try {
        Invoke-WebRequest -UseBasicParsing -Uri "$ReleaseBase/$AssetName" -OutFile $TemporaryArchivePath
        if (-not (Test-FileSha256 $TemporaryArchivePath $ExpectedArchiveSha256)) {
            throw 'The FFmpeg archive failed SHA-256 verification.'
        }
        $ArchiveToExtract = $TemporaryArchivePath
    } catch {
        Remove-Item -LiteralPath $TemporaryArchivePath -Force -ErrorAction SilentlyContinue
        throw
    }
}

if (Test-Path -LiteralPath $DependencyRoot) { Remove-Item -LiteralPath $DependencyRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $DependencyRoot | Out-Null
try {
    Expand-Archive -LiteralPath $ArchiveToExtract -DestinationPath $DependencyRoot -Force
} finally {
    if ($ArchiveToExtract -eq $TemporaryArchivePath) {
        Remove-Item -LiteralPath $TemporaryArchivePath -Force -ErrorAction SilentlyContinue
    }
}
$Resolved = Find-FfmpegRoot $DependencyRoot
if (-not $Resolved) { throw 'The verified FFmpeg archive does not contain the required headers, import libraries and DLLs.' }
Write-Output $Resolved
