[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallRoot,
    [Parameter(Mandatory)][string]$Manifest
)

$ErrorActionPreference = 'Stop'
$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
$Manifest = [IO.Path]::GetFullPath($Manifest)
$UpdateRoot = Join-Path $InstallRoot 'update'
$PackageZip = Join-Path $UpdateRoot 'package.zip'
$ExtractRoot = Join-Path $UpdateRoot 'package'
$AllowedPrefix = $InstallRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not $UpdateRoot.StartsWith($AllowedPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe update path.' }

$Info = Get-Content -Raw -LiteralPath $Manifest | ConvertFrom-Json
if ([string]$Info.packageSha256 -notmatch '^[0-9A-Fa-f]{64}$') { throw 'Invalid package SHA-256 in release manifest.' }
New-Item -ItemType Directory -Force -Path $UpdateRoot | Out-Null
if (Test-Path -LiteralPath $ExtractRoot) { Remove-Item -LiteralPath $ExtractRoot -Recurse -Force }

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -UseBasicParsing -Uri ([string]$Info.packageUrl) -OutFile $PackageZip -TimeoutSec 120
$ActualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $PackageZip).Hash
if ($ActualHash -ne ([string]$Info.packageSha256).ToUpperInvariant()) {
    Remove-Item -LiteralPath $PackageZip -Force
    throw 'Downloaded update SHA-256 does not match the release manifest.'
}

Expand-Archive -LiteralPath $PackageZip -DestinationPath $ExtractRoot -Force
$Updater = Get-ChildItem -LiteralPath $ExtractRoot -Recurse -File -Filter 'update.bat' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.DirectoryName 'tools\update_tui.ps1') } |
    Select-Object -First 1
if (-not $Updater) { throw 'The verified package does not contain update.bat and tools\update_tui.ps1.' }
Copy-Item -LiteralPath $Manifest -Destination (Join-Path $Updater.DirectoryName 'update-manifest.json') -Force
Set-Content -LiteralPath (Join-Path $UpdateRoot '.verified') -Value $ActualHash -Encoding ascii
Set-Content -LiteralPath (Join-Path $UpdateRoot 'updater_path.txt') -Value $Updater.FullName -Encoding utf8
