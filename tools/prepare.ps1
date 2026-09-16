[CmdletBinding()]
param(
    [switch]$SkipEbootDump,
    [switch]$SkipCoreBuild,
    [switch]$SkipDataExtraction,
    [switch]$InputsValidated,
    [string]$ExistingInstallRoot,
    [switch]$ReuseAotCache,
    [switch]$SkipLauncherBuild,
    [scriptblock]$ProgressAction
)

$ErrorActionPreference = 'Stop'
$PublicRoot = Split-Path -Parent $PSScriptRoot
$SourceData = Join-Path $PublicRoot 'data'
$OutputRoot = if ($ExistingInstallRoot) { [IO.Path]::GetFullPath($ExistingInstallRoot) } else { Join-Path $PublicRoot 'out' }
$OutputData = Join-Path $OutputRoot 'data'
$BuildRoot = Join-Path $PublicRoot 'tools\.build\launcher'

function Set-PreparationProgress([int]$Percent, [string]$Activity) {
    if ($ProgressAction) { & $ProgressAction $Percent $Activity }
    else { Write-Progress -Id 1 -Activity 'Preparing FateUnlimitedCodesRecomp' -Status $Activity -PercentComplete $Percent }
}

if (-not $InputsValidated -and -not $ExistingInstallRoot) {
    Set-PreparationProgress 1 'Validating inputs and build tools...'
    . (Join-Path $PSScriptRoot 'validate_inputs.ps1')
    $Validation = Test-FucPublicInputs -InputRoot $SourceData
    if (-not $Validation.IsValid) {
        $Failures = @($Validation.Items | Where-Object State -ne 'Ok' | ForEach-Object { "$($_.Name): $($_.Detail)" })
        throw ("Input validation failed:`n - " + ($Failures -join "`n - "))
    }
}

$Iso = if ($ExistingInstallRoot) { Get-Item -LiteralPath (Join-Path $OutputData 'game.iso') -ErrorAction SilentlyContinue } else { Get-Item -LiteralPath (Join-Path $SourceData 'game.iso') -ErrorAction SilentlyContinue }
$FontZip = if (-not $ExistingInstallRoot) { Get-Item -LiteralPath (Join-Path $SourceData 'font.zip') -ErrorAction SilentlyContinue }
$PpssppZip = if (-not $ExistingInstallRoot) { Get-Item -LiteralPath (Join-Path $SourceData 'ppsspp.zip') -ErrorAction SilentlyContinue }
if (-not $ExistingInstallRoot -and (-not $Iso -or -not $FontZip -or -not $PpssppZip)) {
    throw 'Place game.iso, font.zip and ppsspp.zip directly in public\data using these exact file names.'
}

if (-not $SkipLauncherBuild) {
    Set-PreparationProgress 5 'Building the launcher and ISO data tool...'
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release
    if ($LASTEXITCODE -ne 0) { throw 'Launcher build failed.' }
}
New-Item -ItemType Directory -Force -Path (Join-Path $OutputData 'game'),(Join-Path $OutputData 'fonts'),(Join-Path $OutputData 'eboot'),(Join-Path $OutputData 'runtime') | Out-Null

$DataTool = Join-Path $BuildRoot 'bin\FateUnlimitedCodesDataTool.exe'
if (-not $SkipDataExtraction) {
    Set-PreparationProgress 18 'Extracting the verified PSP ISO...'
    & $DataTool extract-iso $Iso.FullName (Join-Path $OutputData 'game')
    if ($LASTEXITCODE -ne 0) { throw 'ISO extraction failed.' }
    Copy-Item -LiteralPath $Iso.FullName -Destination (Join-Path $OutputData 'game.iso') -Force
    Set-PreparationProgress 30 'Extracting the 18 PSP fonts...'
    Expand-Archive -LiteralPath $FontZip.FullName -DestinationPath (Join-Path $OutputData 'fonts') -Force
}

$Eboot = Join-Path $OutputData 'eboot\NPEH00007_EBOOT_decrypted.BIN'
if (-not $SkipEbootDump) {
    Set-PreparationProgress 38 'Automatically dumping the decrypted EBOOT with the supplied PPSSPP...'
    & (Join-Path $PSScriptRoot 'decrypt_eboot.ps1') -Iso $Iso.FullName -PpssppZip $PpssppZip.FullName -OutputEboot $Eboot
}
if (-not (Test-Path -LiteralPath $Eboot -PathType Leaf)) { throw "Decrypted EBOOT is missing: $Eboot" }
$ExpectedEbootSha256 = '33E8240519A232BE40BE503F17A23C2DFD5A361177FFB925DE444DF95DBE6BA7'
$EbootSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $Eboot).Hash
if ($EbootSha256 -ne $ExpectedEbootSha256) {
    throw 'The decrypted EBOOT does not match the supported NPEH00007 1.01 executable.'
}

if (-not $SkipCoreBuild) {
    $FrameworkBuild = Join-Path $PublicRoot 'tools\.build\psprecomp'
    $FrameworkRecompiler = Join-Path $FrameworkBuild 'psp_recomp.exe'
    $FrameworkCore = Join-Path $FrameworkBuild 'libpsprecomp_core.a'
    $NeedsFrameworkBuild = -not (Test-Path -LiteralPath $FrameworkCore -PathType Leaf) -or
        (-not $ReuseAotCache -and -not (Test-Path -LiteralPath $FrameworkRecompiler -PathType Leaf))
    if ($NeedsFrameworkBuild) {
        Set-PreparationProgress 50 'Building PSPRecomp...'
        & (Join-Path $PSScriptRoot 'build_psprecomp.ps1') | Out-Host
    }
    # Every generated/private path is rooted beside run.bat. Never derive a
    # parent workspace and append a literal `public`, because a downloaded
    # repository may have any directory name.
    $Generated = if ($ReuseAotCache) {
        Join-Path $OutputData 'recomp-cache\generated'
    } else {
        Join-Path $PublicRoot 'tools\.build\generated'
    }
    if ($ReuseAotCache -and -not (Get-ChildItem -LiteralPath $Generated -File -Filter '*.cpp' -ErrorAction SilentlyContinue | Select-Object -First 1)) {
        throw 'The private AOT cache is missing; this update must request AOT regeneration.'
    }
    $PreviousAllLabels = $env:PSPRECOMP_ALL_LABELS_ENTRIES
    try {
        $env:PSPRECOMP_ALL_LABELS_ENTRIES = '1'
        if (-not $ReuseAotCache) {
            Set-PreparationProgress 60 'Generating the static recompilation corpus...'
            & $FrameworkRecompiler $Eboot --auto $Generated 0x08804000 16384
            if ($LASTEXITCODE -ne 0) { throw 'PSPRecomp AOT generation failed.' }
            $AotCache = Join-Path $OutputData 'recomp-cache\generated'
            if (Test-Path -LiteralPath $AotCache) { Remove-Item -LiteralPath $AotCache -Recurse -Force }
            New-Item -ItemType Directory -Force -Path $AotCache | Out-Null
            Copy-Item -Path (Join-Path $Generated '*') -Destination $AotCache -Recurse -Force
        } else {
            Set-PreparationProgress 60 'Reusing the compatible private AOT corpus...'
        }
    } finally {
        if ($null -eq $PreviousAllLabels) { Remove-Item Env:PSPRECOMP_ALL_LABELS_ENTRIES -ErrorAction SilentlyContinue }
        else { $env:PSPRECOMP_ALL_LABELS_ENTRIES = $PreviousAllLabels }
    }
    $CoreBuilder = Join-Path $PublicRoot 'tools\build_psp_core.ps1'
    if (-not (Test-Path -LiteralPath $CoreBuilder -PathType Leaf)) {
        throw 'The public native PSP core builder is missing from tools\build_psp_core.ps1.'
    }
    Set-PreparationProgress 78 'Compiling and linking the native PSP core...'
    & $CoreBuilder -Release -Corpus $Generated `
        -WorkDir (Join-Path $OutputData 'recomp-cache\objects') `
        -Output (Join-Path $OutputData 'runtime\FateUnlimitedCodesRecompCore.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Static core build failed.' }
    Copy-Item -LiteralPath (Join-Path $PublicRoot 'RUNTIME_NOTICE.txt') `
        -Destination (Join-Path $OutputData 'runtime\README.txt') -Force
    $LegacyNotice = Join-Path $OutputData 'runtime\LEEME.txt'
    if (Test-Path -LiteralPath $LegacyNotice) { Remove-Item -LiteralPath $LegacyNotice -Force }
}

Set-PreparationProgress 96 'Writing the private output manifest...'
$PreviousPreparation = $null
$PreviousPreparationPath = Join-Path $OutputData 'preparation.json'
if ($ExistingInstallRoot -and (Test-Path -LiteralPath $PreviousPreparationPath)) {
    $PreviousPreparation = Get-Content -Raw -LiteralPath $PreviousPreparationPath | ConvertFrom-Json
}
@{
    preparedUtc = [DateTime]::UtcNow.ToString('o')
    isoSha256 = if ($Iso) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Iso.FullName).Hash } else { [string]$PreviousPreparation.isoSha256 }
    ebootSha256 = $EbootSha256
    ppssppZipSha256 = if ($PpssppZip) { (Get-FileHash -Algorithm SHA256 -LiteralPath $PpssppZip.FullName).Hash } else { [string]$PreviousPreparation.ppssppZipSha256 }
    sourceRoot = $PublicRoot
    version = (Get-Content -Raw -LiteralPath (Join-Path $PublicRoot 'version.txt')).Trim()
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputData 'preparation.json') -Encoding utf8

Write-Host "Ready-to-run private output: $OutputRoot"
Set-PreparationProgress 100 'Complete.'
if (-not $ProgressAction) { Write-Progress -Id 1 -Activity 'Preparing FateUnlimitedCodesRecomp' -Completed }
