[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
$PublicRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = Join-Path $PublicRoot 'tools\.build\launcher'
$OutputRoot = Join-Path $PublicRoot 'out'

. (Join-Path $PSScriptRoot 'toolchain.ps1')
$Toolchain = Get-FucToolchain
Reset-FucBuildDirectoryIfGeneratorChanged $BuildRoot $Toolchain.Generator
& $Toolchain.Cmake -S $PublicRoot -B $BuildRoot -G $Toolchain.Generator `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DCMAKE_CXX_COMPILER=$($Toolchain.Compiler)" `
    "-DCMAKE_MAKE_PROGRAM=$($Toolchain.Make)"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $Toolchain.Cmake --build $BuildRoot --parallel
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
foreach ($Tool in @('check_update.ps1','download_update.ps1','update_tui.ps1','cleanup_update.ps1','toolchain.ps1','ensure_toolchain.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $Tool) -Destination $OutputTools -Force
}

Write-Host "Launcher built: $(Join-Path $OutputRoot 'FateUnlimitedCodesRecomp.exe')"
Write-Host "Data tool built: $(Join-Path $BinRoot 'FateUnlimitedCodesDataTool.exe')"
