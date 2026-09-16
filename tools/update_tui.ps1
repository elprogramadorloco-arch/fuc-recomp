[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallRoot,
    [string]$Manifest,
    [switch]$NoPause,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
$PackageRoot = Split-Path -Parent $PSScriptRoot
$InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
if (-not $Manifest) { $Manifest = Join-Path $PackageRoot 'update-manifest.json' }
$Manifest = [IO.Path]::GetFullPath($Manifest)
$UpdateRoot = Join-Path $InstallRoot 'update'
$SourceRoot = Split-Path -Parent $InstallRoot
$ExpectedEbootSha256 = '33E8240519A232BE40BE503F17A23C2DFD5A361177FFB925DE444DF95DBE6BA7'
$RequiredFonts = @('jpn0.pgf','kr0.pgf') + (0..15 | ForEach-Object { "ltn$_.pgf" })

function Show-Update([int]$Percent, [string]$Activity, [string[]]$Lines = @()) {
    Clear-Host
    Write-Host ('=' * 78) -ForegroundColor DarkCyan
    Write-Host '                         FateUnlimitedCodesRecomp Update' -ForegroundColor Cyan
    Write-Host ('=' * 78) -ForegroundColor DarkCyan
    Write-Host
    $Done = [Math]::Floor([Math]::Max(0,[Math]::Min(100,$Percent)) / 4)
    $Bar = ('#' * $Done) + ('-' * (25 - $Done))
    Write-Host ("  [{0}] {1,3}%" -f $Bar,$Percent) -ForegroundColor Green
    Write-Host "  $Activity" -ForegroundColor Yellow
    Write-Host
    foreach($Line in $Lines){ Write-Host "  $Line" }
    Write-Progress -Id 2 -Activity 'Updating FateUnlimitedCodesRecomp' -Status $Activity -PercentComplete $Percent
}

function Get-BuildEnvironment {
    try {
        . (Join-Path $PSScriptRoot 'toolchain.ps1')
        $Toolchain = Get-FucToolchain
        [pscustomobject]@{ Valid=$true; Cmake=$Toolchain.Cmake; Compiler=$Toolchain.Compiler; Error='' }
    } catch {
        [pscustomobject]@{ Valid=$false; Cmake=$null; Compiler=$null; Error=$_.Exception.Message }
    }
}

function Copy-PublicSource([string]$From, [string]$To) {
    $From = [IO.Path]::GetFullPath($From).TrimEnd([IO.Path]::DirectorySeparatorChar)
    Get-ChildItem -LiteralPath $From -Recurse -Force -File | ForEach-Object {
        $Relative = $_.FullName.Substring($From.Length).TrimStart([IO.Path]::DirectorySeparatorChar)
        if($Relative -match '^(data|out|\.git|update)([\\/]|$)' -or
           $Relative -match '^tools[\\/]\.build([\\/]|$)'){return}
        $Destination = Join-Path $To $Relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Force
    }
}

try {
    Show-Update 2 'Reading the verified release manifest...'
    if(-not (Test-Path -LiteralPath $Manifest -PathType Leaf)){throw 'update-manifest.json is missing.'}
    $Info = Get-Content -Raw -LiteralPath $Manifest | ConvertFrom-Json
    $PackageVersionPath = Join-Path $PackageRoot 'version.txt'
    if(-not (Test-Path -LiteralPath $PackageVersionPath -PathType Leaf) -or
       (Get-Content -Raw -LiteralPath $PackageVersionPath).Trim() -ne [string]$Info.version){
        throw 'The package version does not match the release manifest.'
    }
    $VerifiedMarker = Join-Path $UpdateRoot '.verified'
    if(-not (Test-Path -LiteralPath $VerifiedMarker -PathType Leaf)){throw 'The downloaded package has no SHA-256 verification marker.'}
    $VerifiedHash = (Get-Content -Raw -LiteralPath $VerifiedMarker).Trim()
    if($VerifiedHash -ne ([string]$Info.packageSha256).ToUpperInvariant()){throw 'The extracted package verification marker does not match this manifest.'}

    Show-Update 8 'Validating the installed private PSP data...'
    $Eboot = Join-Path $InstallRoot 'data\eboot\NPEH00007_EBOOT_decrypted.BIN'
    if(-not (Test-Path -LiteralPath $Eboot -PathType Leaf) -or (Get-FileHash -Algorithm SHA256 -LiteralPath $Eboot).Hash -ne $ExpectedEbootSha256){throw 'The installed private EBOOT is missing or invalid.'}
    if(-not (Test-Path -LiteralPath (Join-Path $InstallRoot 'data\game\PSP_GAME') -PathType Container)){throw 'Prepared PSP game data is missing.'}
    $FontNames = @(Get-ChildItem -LiteralPath (Join-Path $InstallRoot 'data\fonts') -Recurse -File -Filter '*.pgf' -ErrorAction SilentlyContinue | ForEach-Object {$_.Name.ToLowerInvariant()})
    $MissingFonts = @($RequiredFonts | Where-Object {$FontNames -notcontains $_})
    if($MissingFonts){throw "Installed PSP fonts are incomplete: $($MissingFonts -join ', ')"}

    Show-Update 16 'Validating the installed build toolchain...'
    $BuildEnvironment = Get-BuildEnvironment
    if(-not $BuildEnvironment.Valid){throw "CMake 3.22+ and the verified portable LLVM-MinGW toolchain are required. $($BuildEnvironment.Error)"}

    Show-Update 22 'Checking temporary disk space...'
    $RequiredBytes = [int64]$Info.requiredFreeBytes
    $Drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($InstallRoot))
    if($Drive.AvailableFreeSpace -lt $RequiredBytes){throw "Not enough free space. Required: $RequiredBytes bytes."}

    $LauncherChanged = [bool]$Info.components.launcher
    $RuntimeChanged = [bool]$Info.components.runtime
    $AotMode = [string]$Info.components.aot
    if($AotMode -notin @('reuse','regenerate')){throw 'Unknown AOT update mode.'}
    $Plan = @(
        "Version: $($Info.version)",
        "Launcher: $(if($LauncherChanged){'BUILD'}else{'SKIP'})",
        "Native runtime: $(if($RuntimeChanged){'BUILD'}else{'SKIP'})",
        "AOT corpus: $($AotMode.ToUpperInvariant())"
    )
    Show-Update 30 'Incremental update plan is ready.' $Plan
    if($ValidateOnly){ Write-Progress -Id 2 -Activity 'Updating FateUnlimitedCodesRecomp' -Completed; if(-not $NoPause){Read-Host 'Press Enter to close'|Out-Null}; exit 0 }

    $Backup = Join-Path $UpdateRoot 'backup'
    Show-Update 38 'Backing up the active launcher and runtime...'
    New-Item -ItemType Directory -Force -Path $Backup | Out-Null
    foreach($Relative in @('FateUnlimitedCodesRecomp.exe','data\runtime\FateUnlimitedCodesRecompCore.exe','version.txt')){
        $Source=Join-Path $InstallRoot $Relative; if(Test-Path -LiteralPath $Source){$Dest=Join-Path $Backup $Relative;New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Dest)|Out-Null;Copy-Item -LiteralPath $Source -Destination $Dest -Force}
    }

    Show-Update 46 'Installing the new public source files...'
    if(-not (Test-Path -LiteralPath (Join-Path $SourceRoot 'run.bat'))){throw 'The installed source root was not found next to the launcher folder.'}
    Copy-PublicSource $PackageRoot $SourceRoot

    if($LauncherChanged){
        Show-Update 58 'Compiling the updated launcher...'
        & (Join-Path $SourceRoot 'tools\build.ps1') -Configuration Release
        if($LASTEXITCODE -ne 0){throw 'Launcher compilation failed.'}
    }
    if($RuntimeChanged -or $AotMode -eq 'regenerate'){
        Show-Update 68 $(if($AotMode -eq 'regenerate'){'Regenerating AOT and compiling the runtime...'}else{'Reusing AOT and compiling changed runtime objects...'})
        $PrepareArgs = @{ExistingInstallRoot=$InstallRoot; InputsValidated=$true; SkipDataExtraction=$true; SkipEbootDump=$true}
        if($AotMode -eq 'reuse'){$PrepareArgs.ReuseAotCache=$true}
        if(-not $LauncherChanged){$PrepareArgs.SkipLauncherBuild=$true}
        & (Join-Path $SourceRoot 'tools\prepare.ps1') @PrepareArgs
        if($LASTEXITCODE -ne 0){throw 'Native runtime update failed.'}
    }

    Show-Update 92 'Writing update state...'
    $StateRoot = Join-Path $InstallRoot 'data\recomp-cache'
    New-Item -ItemType Directory -Force -Path $StateRoot | Out-Null
    @{version=[string]$Info.version;updatedUtc=[DateTime]::UtcNow.ToString('o');aot=$AotMode} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $StateRoot 'state.json') -Encoding utf8
    Set-Content -LiteralPath (Join-Path $InstallRoot 'version.txt') -Value ([string]$Info.version) -Encoding ascii
    Write-Progress -Id 2 -Activity 'Updating FateUnlimitedCodesRecomp' -Completed
    Show-Update 100 'Update completed successfully.' @('Your PSP data and save folder were preserved.','You can now start the launcher again.')
    if(-not $NoPause){Read-Host 'Press Enter to close'|Out-Null}
    $CleanupRoot = Join-Path $InstallRoot 'save\update'
    New-Item -ItemType Directory -Force -Path $CleanupRoot | Out-Null
    $CleanupScript = Join-Path $CleanupRoot 'cleanup_update.ps1'
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'cleanup_update.ps1') -Destination $CleanupScript -Force
    Start-Process powershell.exe -WindowStyle Hidden -ArgumentList @(
        '-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$CleanupScript`"",
        '-InstallRoot',"`"$InstallRoot`"",'-UpdateRoot',"`"$UpdateRoot`""
    ) | Out-Null
} catch {
    Write-Progress -Id 2 -Activity 'Updating FateUnlimitedCodesRecomp' -Completed
    $Backup = Join-Path $UpdateRoot 'backup'
    if(Test-Path -LiteralPath $Backup){
        Get-ChildItem -LiteralPath $Backup -Recurse -File | ForEach-Object {$Relative=$_.FullName.Substring($Backup.Length).TrimStart('\');$Dest=Join-Path $InstallRoot $Relative;New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Dest)|Out-Null;Copy-Item -LiteralPath $_.FullName -Destination $Dest -Force}
    }
    Write-Host; Write-Host "Update failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'The previous launcher/runtime backup was restored.' -ForegroundColor Yellow
    if(-not $NoPause){Read-Host 'Press Enter to close'|Out-Null}
    exit 1
}
