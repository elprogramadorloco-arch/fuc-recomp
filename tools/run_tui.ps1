[CmdletBinding()]
param(
    [switch]$NoPause,
    [switch]$ValidateOnly,
    [string]$DataRoot
)

$ErrorActionPreference = 'Stop'
$PublicRoot = Split-Path -Parent $PSScriptRoot
if (-not $DataRoot) { $DataRoot = Join-Path $PublicRoot 'data' }
$RequestedDataRoot = [IO.Path]::GetFullPath($DataRoot)
$ValidationScript = Join-Path $PSScriptRoot 'validate_inputs.ps1'
$PrepareScript = Join-Path $PSScriptRoot 'prepare.ps1'
. $ValidationScript -DataRoot $RequestedDataRoot
$DataRoot = $RequestedDataRoot

function Write-Centered([string]$Text, [ConsoleColor]$Color = [ConsoleColor]::Gray) {
    $Width = [Math]::Max(60, [Math]::Min(100, $Host.UI.RawUI.WindowSize.Width - 1))
    $Padding = [Math]::Max(0, [int](($Width - $Text.Length) / 2))
    Write-Host ((' ' * $Padding) + $Text) -ForegroundColor $Color
}

function Show-Header([string]$Subtitle) {
    Clear-Host
    Write-Host ('=' * 78) -ForegroundColor DarkCyan
    Write-Centered 'FateUnlimitedCodesRecomp' Cyan
    Write-Centered $Subtitle DarkGray
    Write-Host ('=' * 78) -ForegroundColor DarkCyan
    Write-Host
}

function Show-Validation([object]$Result, [string]$Activity = '') {
    Show-Header 'Local static recompilation preparation'
    if ($Activity) {
        Write-Host "  [....] $Activity" -ForegroundColor Yellow
        Write-Host
    }
    if ($Result) {
        foreach ($Item in $Result.Items) {
            $Tag = switch ($Item.State) { 'Ok' {' OK '} 'Missing' {'MISS'} default {'FAIL'} }
            $Color = switch ($Item.State) { 'Ok' {'Green'} 'Missing' {'Yellow'} default {'Red'} }
            Write-Host "  [$Tag] " -NoNewline -ForegroundColor $Color
            Write-Host $Item.Name -NoNewline -ForegroundColor White
            Write-Host " - $($Item.Detail)" -ForegroundColor DarkGray
        }
        Write-Host
    }
}

$Pending = [pscustomobject]@{ Items = @(); IsValid = $false }
$Status = { param($Text) Show-Validation $Pending $Text }
try {
    Show-Validation $Pending 'Starting validation...'
    $Validation = Test-FucPublicInputs -InputRoot $DataRoot -OnStatus $Status
    Show-Validation $Validation
    if (-not $Validation.IsValid) {
        Write-Host '  Requirements are not satisfied. Fix the items marked MISS or FAIL and run again.' -ForegroundColor Red
        Write-Host "  Input folder: $DataRoot" -ForegroundColor DarkGray
        if (-not $NoPause) { Write-Host; Read-Host 'Press Enter to close' | Out-Null }
        exit 1
    }

    Write-Host '  All required inputs and build tools are valid.' -ForegroundColor Green
    if ($ValidateOnly) {
        Write-Host '  Validation-only mode: compilation was not started.' -ForegroundColor Yellow
        if (-not $NoPause) { Write-Host; Read-Host 'Press Enter to close' | Out-Null }
        exit 0
    }
    Write-Host '  Compilation will now create private local files under out and tools\.build.' -ForegroundColor DarkGray
    Write-Host

    $ProgressAction = {
        param([int]$Percent, [string]$Activity)
        Write-Progress -Id 1 -Activity 'Building FateUnlimitedCodesRecomp' `
            -Status $Activity -PercentComplete ([Math]::Max(0, [Math]::Min(100, $Percent)))
    }
    & $PrepareScript -InputsValidated -ProgressAction $ProgressAction
    Write-Progress -Id 1 -Activity 'Building FateUnlimitedCodesRecomp' -Completed

    Show-Header 'Preparation complete'
    Write-Centered '[##############################] 100%' Green
    Write-Host
    Write-Centered 'The executable has been created in out.' Green
    Write-Centered 'Private generated files must not be committed or redistributed.' Yellow
    if (-not $NoPause) { Write-Host; Read-Host 'Press Enter to close' | Out-Null }
} catch {
    Write-Progress -Id 1 -Activity 'Building FateUnlimitedCodesRecomp' -Completed
    Write-Host
    Write-Host "  Build failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host '  Correct the reported problem and run run.bat again.' -ForegroundColor Yellow
    if (-not $NoPause) { Write-Host; Read-Host 'Press Enter to close' | Out-Null }
    exit 1
}
