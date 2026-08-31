[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$PublicRoot = Split-Path -Parent $PSScriptRoot
$DependencyRoot = Join-Path $PublicRoot 'tools\.build\dependencies\ffmpeg'
$ArchiveRoot = Join-Path $PublicRoot 'tools\.build\downloads'
$AssetName = 'ffmpeg-N-126313-g1ae4048218-win64-lgpl-shared.zip'
$ReleaseBase = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-08-29-13-12'
$ExpectedArchiveSha256 = '353A95C745D10E52F0E32798D0A72DDE2C1F387D3C22F40EC94BF953ADA5E6FE'

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
$ArchivePath = Join-Path $ArchiveRoot $AssetName
$TemporaryArchivePath = "$ArchivePath.$PID.partial.zip"

function Test-FileSha256([string]$Path, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    try { return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash -eq $Expected }
    catch [System.IO.IOException] { return $false }
}

function Invoke-VerifiedDownload([string]$Uri, [string]$Destination, [string]$ExpectedSha256) {
    $Curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    $LastFailure = $null
    $RetryDelays = @(2,5,10)
    foreach ($Attempt in 1..4) {
        Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
        Write-Host "Downloading pinned FFmpeg LGPL package (attempt $Attempt of 4)..."
        try {
            if ($Curl) {
                & $Curl.Source --location --fail --show-error --silent `
                    --connect-timeout 20 --max-time 300 `
                    --output $Destination $Uri
                if ($LASTEXITCODE -ne 0) { throw "curl.exe exited with code $LASTEXITCODE." }
            } else {
                [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
                Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $Destination -TimeoutSec 300
            }
            if (-not (Test-FileSha256 $Destination $ExpectedSha256)) {
                throw 'The downloaded FFmpeg archive failed its pinned SHA-256 verification.'
            }
            return
        } catch {
            $LastFailure = $_.Exception.Message
            Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
            if ($Attempt -lt 4) { Start-Sleep -Seconds $RetryDelays[$Attempt - 1] }
        }
    }
    throw "Unable to download the pinned FFmpeg LGPL package after 4 attempts. Check the Internet connection, firewall or proxy and run run.bat again. Last error: $LastFailure"
}

if (Test-FileSha256 $ArchivePath $ExpectedArchiveSha256) {
    $ArchiveToExtract = $ArchivePath
} else {
    try {
        Invoke-VerifiedDownload "$ReleaseBase/$AssetName" $TemporaryArchivePath $ExpectedArchiveSha256
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
