[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$PublicRoot = Split-Path -Parent $PSScriptRoot
$DependencyRoot = Join-Path $PublicRoot 'tools\.build\dependencies\llvm-mingw'
$ArchiveRoot = Join-Path $PublicRoot 'tools\.build\downloads'
$AssetName = 'llvm-mingw-20260908-ucrt-x86_64.zip'
$ReleaseBase = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20260908'
$ExpectedArchiveSha256 = '1BCF74D06B724AEecAA6412CA85F5B26FB1DA770E7CDCEFA9263C9C5C3AD34B6'

function Find-LlvmMingwRoot([string]$SearchRoot) {
    $Compiler = Get-ChildItem -LiteralPath $SearchRoot -Recurse -File -Filter clang++.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq 'bin' } | Select-Object -First 1
    if (-not $Compiler) { return $null }
    $Root = Split-Path -Parent $Compiler.Directory.FullName
    foreach ($Required in @('bin\clang++.exe','bin\mingw32-make.exe','bin\llvm-ar.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $Required) -PathType Leaf)) { return $null }
    }
    return $Root
}

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
        Write-Host "Downloading pinned LLVM-MinGW toolchain (attempt $Attempt of 4)..."
        try {
            if ($Curl) {
                & $Curl.Source --location --fail --show-error --silent `
                    --connect-timeout 20 --max-time 600 --output $Destination $Uri
                if ($LASTEXITCODE -ne 0) { throw "curl.exe exited with code $LASTEXITCODE." }
            } else {
                [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
                Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $Destination -TimeoutSec 600
            }
            if (-not (Test-FileSha256 $Destination $ExpectedSha256)) {
                throw 'The downloaded LLVM-MinGW archive failed its pinned SHA-256 verification.'
            }
            return
        } catch {
            $LastFailure = $_.Exception.Message
            Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
            if ($Attempt -lt 4) { Start-Sleep -Seconds $RetryDelays[$Attempt - 1] }
        }
    }
    throw "Unable to download the pinned LLVM-MinGW toolchain after 4 attempts. Check the Internet connection, firewall or proxy and run run.bat again. Last error: $LastFailure"
}

$Existing = Find-LlvmMingwRoot $DependencyRoot
if ($Existing) { Write-Output $Existing; exit 0 }

New-Item -ItemType Directory -Force -Path $ArchiveRoot | Out-Null
$ArchivePath = Join-Path $ArchiveRoot $AssetName
$TemporaryArchivePath = "$ArchivePath.$PID.partial.zip"
if (Test-FileSha256 $ArchivePath $ExpectedArchiveSha256) {
    $ArchiveToExtract = $ArchivePath
} else {
    try {
        Invoke-VerifiedDownload "$ReleaseBase/$AssetName" $TemporaryArchivePath $ExpectedArchiveSha256
        Move-Item -LiteralPath $TemporaryArchivePath -Destination $ArchivePath -Force
        $ArchiveToExtract = $ArchivePath
    } catch {
        Remove-Item -LiteralPath $TemporaryArchivePath -Force -ErrorAction SilentlyContinue
        throw
    }
}

if (Test-Path -LiteralPath $DependencyRoot) { Remove-Item -LiteralPath $DependencyRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $DependencyRoot | Out-Null
Expand-Archive -LiteralPath $ArchiveToExtract -DestinationPath $DependencyRoot -Force
$Resolved = Find-LlvmMingwRoot $DependencyRoot
if (-not $Resolved) { throw 'The verified LLVM-MinGW archive does not contain the required x64 compiler and build tools.' }
Write-Output $Resolved
