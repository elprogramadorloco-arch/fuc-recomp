[CmdletBinding()]
param(
    [string]$DataRoot,
    [scriptblock]$StatusAction,
    [switch]$PassThru
)

$ErrorActionPreference = 'Stop'
$PublicRoot = Split-Path -Parent $PSScriptRoot
if (-not $DataRoot) { $DataRoot = Join-Path $PublicRoot 'data' }

$ExpectedIsoSha256 = 'BDF13BEFFBAD519FD51F1AB075D165D3C7BFA075FEEF1A5B1FD7775BAC0F830D'
$RequiredFonts = @('jpn0.pgf','kr0.pgf') + (0..15 | ForEach-Object { "ltn$_.pgf" })

function Send-ValidationStatus([string]$Text) {
    if ($StatusAction) { & $StatusAction $Text }
}

function New-ValidationItem([string]$Name, [string]$State, [string]$Detail, [string]$Path = '') {
    [pscustomobject]@{ Name = $Name; State = $State; Detail = $Detail; Path = $Path }
}

function Find-SingleFile([string]$Label, [scriptblock]$Predicate) {
    $Matches = @(Get-ChildItem -LiteralPath $DataRoot -File -ErrorAction SilentlyContinue | Where-Object $Predicate)
    if ($Matches.Count -eq 0) {
        return [pscustomobject]@{ Item = (New-ValidationItem $Label 'Missing' 'Required file was not found.'); File = $null }
    }
    if ($Matches.Count -gt 1) {
        return [pscustomobject]@{ Item = (New-ValidationItem $Label 'Invalid' 'More than one candidate was found; leave exactly one.'); File = $null }
    }
    [pscustomobject]@{ Item = $null; File = $Matches[0] }
}

function Test-FucPublicInputs {
    [CmdletBinding()]
    param([string]$InputRoot = $DataRoot, [scriptblock]$OnStatus = $StatusAction)

    $script:DataRoot = [IO.Path]::GetFullPath($InputRoot)
    $script:StatusAction = $OnStatus
    $Items = [Collections.Generic.List[object]]::new()

    if (-not [Environment]::Is64BitOperatingSystem) {
        $Items.Add((New-ValidationItem 'Windows x64' 'Invalid' 'A 64-bit Windows installation is required.'))
    } else {
        $Items.Add((New-ValidationItem 'Windows x64' 'Ok' '64-bit Windows detected.'))
    }

    . (Join-Path $PSScriptRoot 'toolchain.ps1')
    $Toolchain = $null
    Send-ValidationStatus 'Checking CMake and preparing the portable C++ toolchain...'
    try {
        $Toolchain = Get-FucToolchain
        $Items.Add((New-ValidationItem 'CMake' 'Ok' "CMake $($Toolchain.CmakeVersion) detected." $Toolchain.Cmake))
        $CompilerVersion = (& $Toolchain.Compiler --version | Select-Object -First 1)
        $Items.Add((New-ValidationItem 'C++ toolchain' 'Ok' "Pinned portable LLVM-MinGW x64 toolchain ready ($CompilerVersion)." $Toolchain.Root))
    } catch {
        $Items.Add((New-ValidationItem 'Build environment' 'Missing' $_.Exception.Message))
    }

    Send-ValidationStatus 'Locating data\game.iso...'
    $IsoResult = Find-SingleFile 'game.iso' { $_.Name -ieq 'game.iso' }
    $Iso = $IsoResult.File
    if ($IsoResult.Item) {
        $Items.Add($IsoResult.Item)
    } elseif ($Iso.Length -ne 738394112) {
        $Items.Add((New-ValidationItem 'game.iso' 'Invalid' 'The ISO size does not match the supported NPEH00007 1.01 dump.' $Iso.FullName))
    } else {
        Send-ValidationStatus 'Validating the PSP ISO SHA-256...'
        $IsoHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Iso.FullName).Hash
        if ($IsoHash -ne $ExpectedIsoSha256) {
            $Items.Add((New-ValidationItem 'game.iso' 'Invalid' 'This is not the verified NPEH00007 1.01 ISO required by this recompilation profile.' $Iso.FullName))
        } else {
            $Items.Add((New-ValidationItem 'game.iso' 'Ok' 'Verified Fate/unlimited codes NPEH00007 1.01 ISO.' $Iso.FullName))
        }
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    Send-ValidationStatus 'Checking font.zip and all required PGF fonts...'
    $FontResult = Find-SingleFile 'font.zip' { $_.Name -ieq 'font.zip' }
    $FontZip = $FontResult.File
    if ($FontResult.Item) {
        $Items.Add($FontResult.Item)
    } else {
        try {
            $Archive = [IO.Compression.ZipFile]::OpenRead($FontZip.FullName)
            try {
                $Names = @($Archive.Entries | Where-Object { $_.Name } | ForEach-Object { $_.Name.ToLowerInvariant() })
                $MissingFonts = @($RequiredFonts | Where-Object { $Names -notcontains $_ })
                $EmptyFonts = @($Archive.Entries | Where-Object { $RequiredFonts -contains $_.Name.ToLowerInvariant() -and $_.Length -le 0 } | ForEach-Object Name)
                if ($MissingFonts.Count -gt 0) {
                    $Items.Add((New-ValidationItem 'font.zip' 'Invalid' ("Missing fonts: " + ($MissingFonts -join ', ')) $FontZip.FullName))
                } elseif ($EmptyFonts.Count -gt 0) {
                    $Items.Add((New-ValidationItem 'font.zip' 'Invalid' ("Empty fonts: " + ($EmptyFonts -join ', ')) $FontZip.FullName))
                } else {
                    $Items.Add((New-ValidationItem 'font.zip' 'Ok' 'All 18 required PGF fonts are present.' $FontZip.FullName))
                }
            } finally { $Archive.Dispose() }
        } catch {
            $Items.Add((New-ValidationItem 'font.zip' 'Invalid' ("The ZIP cannot be read: " + $_.Exception.Message) $FontZip.FullName))
        }
    }

    Send-ValidationStatus 'Checking the PPSSPP archive...'
    $PpssppResult = Find-SingleFile 'ppsspp.zip' { $_.Name -ieq 'ppsspp.zip' }
    $PpssppZip = $PpssppResult.File
    if ($PpssppResult.Item) {
        $Items.Add($PpssppResult.Item)
    } else {
        try {
            $Archive = [IO.Compression.ZipFile]::OpenRead($PpssppZip.FullName)
            try {
                $Exe = $Archive.Entries | Where-Object { $_.Name -ieq 'PPSSPPWindows64.exe' -and $_.Length -gt 0 } | Select-Object -First 1
                $Assets = $Archive.Entries | Where-Object { $_.FullName -match '^(?i)assets/' -and $_.Length -gt 0 } | Select-Object -First 1
                if (-not $Exe) {
                    $Items.Add((New-ValidationItem 'ppsspp.zip' 'Invalid' 'PPSSPPWindows64.exe is missing from the ZIP.' $PpssppZip.FullName))
                } elseif (-not $Assets) {
                    $Items.Add((New-ValidationItem 'ppsspp.zip' 'Invalid' 'The PPSSPP assets directory is missing or empty.' $PpssppZip.FullName))
                } else {
                    $Items.Add((New-ValidationItem 'ppsspp.zip' 'Ok' 'Windows x64 executable and assets detected.' $PpssppZip.FullName))
                }
            } finally { $Archive.Dispose() }
        } catch {
            $Items.Add((New-ValidationItem 'ppsspp.zip' 'Invalid' ("The ZIP cannot be read: " + $_.Exception.Message) $PpssppZip.FullName))
        }
    }

    [pscustomobject]@{
        IsValid = @($Items | Where-Object State -ne 'Ok').Count -eq 0
        Items = @($Items)
        Iso = $Iso
        FontZip = $FontZip
        PpssppZip = $PpssppZip
        Cmake = if ($Toolchain) { $Toolchain.Cmake } else { $null }
        Toolchain = $Toolchain
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    $Result = Test-FucPublicInputs -InputRoot $DataRoot -OnStatus $StatusAction
    if ($PassThru) { $Result }
    else {
        $Result.Items | Format-Table Name,State,Detail -AutoSize
        if (-not $Result.IsValid) { exit 1 }
    }
}
