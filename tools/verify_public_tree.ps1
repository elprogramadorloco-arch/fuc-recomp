[CmdletBinding()]
param([string]$Root)

$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$RootFull = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar)
$ForbiddenExtensions = @(
    '.iso','.cso','.bin','.pgf','.ppst','.zip','.vag','.at3','.pmf','.fpk',
    '.exe','.dll','.lib','.obj','.pdb','.ilk'
)
$Violations = Get-ChildItem -LiteralPath $Root -Recurse -File | Where-Object {
    $Relative = $_.FullName.Substring($RootFull.Length).TrimStart([IO.Path]::DirectorySeparatorChar)
    $PrivateLocalFile = (
        ($Relative -match '^data[\\/]' -and $Relative -ne 'data\README.txt') -or
        $Relative -match '^(out|tools[\\/]\.build)[\\/]'
    )
    $PrivateLocalFile -or (
        $ForbiddenExtensions -contains $_.Extension.ToLowerInvariant() -or
        $_.Name -eq 'FateUnlimitedCodesRecompCore.exe' -or
        $Relative -match '(^|[\\/])(game|fonts|eboot|runtime|generated)([\\/]|$)'
    )
}

if ($Violations) {
    $Violations | ForEach-Object { Write-Error "Private/generated file: $($_.FullName)" }
    throw 'Public-tree verification failed. Remove private/generated files from the publication copy.'
}
Write-Host 'Public source verification passed.'
Write-Host 'No private inputs, generated output or compiled binaries were found.'
