[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
$PublicRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = Join-Path $PublicRoot 'tools\.build\launcher'
$OutputRoot = Join-Path $PublicRoot 'out'

$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$VsPath = if (Test-Path -LiteralPath $VsWhere -PathType Leaf) {
    & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$VsVersion = if ($VsPath) {
    & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
}
$VsMajor = if ($VsVersion -match '^(\d+)\.') { [int]$Matches[1] } else { 0 }
$Generator = switch ($VsMajor) {
    17 { 'Visual Studio 17 2022' }
    16 { 'Visual Studio 16 2019' }
    default { $null }
}
if (-not $Generator) { throw 'A compatible Visual Studio/Build Tools 2019 or 2022 MSVC x64 toolchain was not found.' }

$Candidates = @(
    (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source,
    (Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'),
    'E:\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
$Cmake = $Candidates | Select-Object -First 1
if (-not $Cmake) { throw 'CMake 3.22 or newer was not found.' }

& $Cmake -S $PublicRoot -B $BuildRoot -G $Generator -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $Cmake --build $BuildRoot --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'Public tools build failed.' }

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $OutputRoot 'data') | Out-Null
$BinRoot = Join-Path $BuildRoot 'bin'
Copy-Item -LiteralPath (Join-Path $BinRoot 'FateUnlimitedCodesRecomp.exe') -Destination $OutputRoot -Force
Copy-Item -LiteralPath (Join-Path $PublicRoot 'README.md') -Destination $OutputRoot -Force
Copy-Item -LiteralPath (Join-Path $PublicRoot 'LEGAL.md') -Destination $OutputRoot -Force
Copy-Item -LiteralPath (Join-Path $PublicRoot 'LICENSE') -Destination $OutputRoot -Force
Copy-Item -LiteralPath (Join-Path $PublicRoot 'update.bat') -Destination $OutputRoot -Force
Copy-Item -LiteralPath (Join-Path $PublicRoot 'version.txt') -Destination $OutputRoot -Force
Copy-Item -LiteralPath (Join-Path $PublicRoot 'update-config.json') -Destination $OutputRoot -Force
$OutputTools = Join-Path $OutputRoot 'tools'
New-Item -ItemType Directory -Force -Path $OutputTools | Out-Null
foreach ($Tool in @('check_update.ps1','download_update.ps1','update_tui.ps1','cleanup_update.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $Tool) -Destination $OutputTools -Force
}

Write-Host "Launcher built: $(Join-Path $OutputRoot 'FateUnlimitedCodesRecomp.exe')"
Write-Host "Data tool built: $(Join-Path $BinRoot 'FateUnlimitedCodesDataTool.exe')"
