[CmdletBinding()]
param([string]$Root)

$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$RootFull = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar)
$ForbiddenExtensions = @(
    '.iso','.cso','.bin','.pgf','.ppst','.zip','.vag','.at3','.pmf','.fpk',
    '.exe','.dll','.lib','.a','.obj','.pdb','.ilk'
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

$RequiredStandaloneSources = @(
    'native\recomp\fate_psp_profile.cpp',
    'native\recomp\psp_entry_host.cpp',
    'native\media\vcs_media_decoder.cpp',
    'native\src\port\psp_atrac.cpp',
    'native\src\renderer\psp_ge_d3d11_surface.cpp',
    'native\src\platform\windows_psp_audio.cpp',
    'tools\build_psp_core.ps1',
    'tools\ensure_ffmpeg.ps1'
)
$MissingStandaloneSources = @($RequiredStandaloneSources | Where-Object {
    -not (Test-Path -LiteralPath (Join-Path $Root $_) -PathType Leaf)
})
if ($MissingStandaloneSources) {
    throw ('Public standalone-build sources are missing: ' + ($MissingStandaloneSources -join ', '))
}

$PrepareText = Get-Content -Raw -LiteralPath (Join-Path $Root 'tools\prepare.ps1')
if ($PrepareText -match '\$WorkspaceRoot' -or $PrepareText -match "'public/tools/\.build") {
    throw 'prepare.ps1 still contains the removed parent-workspace/public path assumption.'
}
Write-Host 'Public source verification passed.'
Write-Host 'No private inputs, generated output or compiled binaries were found.'
Write-Host 'The standalone native PSP host/HLE source set is present.'
