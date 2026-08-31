[CmdletBinding()]
param([Parameter(Mandatory)][string]$InstallRoot)

$ErrorActionPreference = 'Stop'
$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
$ConfigPath = Join-Path $InstallRoot 'update-config.json'
$VersionPath = Join-Path $InstallRoot 'version.txt'
$SaveUpdate = Join-Path $InstallRoot 'save\update'
if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $VersionPath -PathType Leaf)) { exit 0 }

$Config = Get-Content -Raw -LiteralPath $ConfigPath | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace([string]$Config.manifestUrl)) { exit 0 }
$CurrentText = (Get-Content -Raw -LiteralPath $VersionPath).Trim()
try { $Current = [version]$CurrentText } catch { throw "Invalid installed version: $CurrentText" }

New-Item -ItemType Directory -Force -Path $SaveUpdate | Out-Null
$ManifestPath = Join-Path $SaveUpdate 'latest-manifest.json'
$TempPath = "$ManifestPath.download"
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri ([string]$Config.manifestUrl) -OutFile $TempPath -TimeoutSec 8
    $Manifest = Get-Content -Raw -LiteralPath $TempPath | ConvertFrom-Json
    $Latest = [version]([string]$Manifest.version)
    if ([string]::IsNullOrWhiteSpace([string]$Manifest.packageUrl) -or
        [string]$Manifest.packageSha256 -notmatch '^[0-9A-Fa-f]{64}$') {
        throw 'The remote release manifest is incomplete.'
    }
    Move-Item -LiteralPath $TempPath -Destination $ManifestPath -Force
    if ($Latest -le $Current) { exit 0 }
    Set-Content -LiteralPath (Join-Path $SaveUpdate 'latest.version') -Value ([string]$Manifest.version) -Encoding utf8
    Set-Content -LiteralPath (Join-Path $SaveUpdate 'latest.notes') -Value ([string]$Manifest.notes) -Encoding utf8
    exit 10
} catch {
    Remove-Item -LiteralPath $TempPath -Force -ErrorAction SilentlyContinue
    exit 2
}
