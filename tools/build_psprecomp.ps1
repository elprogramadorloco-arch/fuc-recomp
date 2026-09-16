[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$PublicRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = Join-Path $PublicRoot 'tools\.build\psprecomp'
. (Join-Path $PSScriptRoot 'toolchain.ps1')
$Toolchain = Get-FucToolchain
Reset-FucBuildDirectoryIfGeneratorChanged $BuildRoot $Toolchain.Generator

& $Toolchain.Cmake -S (Join-Path $PublicRoot 'PSPRecomp') -B $BuildRoot `
    -G $Toolchain.Generator '-DCMAKE_BUILD_TYPE=Release' `
    "-DCMAKE_CXX_COMPILER=$($Toolchain.Compiler)" `
    "-DCMAKE_MAKE_PROGRAM=$($Toolchain.Make)" '-DPSPRECOMP_PROFILE='
if ($LASTEXITCODE -ne 0) { throw 'PSPRecomp configuration failed.' }
& $Toolchain.Cmake --build $BuildRoot --parallel --target psp_recomp psprecomp_core
if ($LASTEXITCODE -ne 0) { throw 'PSPRecomp build failed.' }

[pscustomobject]@{
    Recompiler = Join-Path $BuildRoot 'psp_recomp.exe'
    CoreLibrary = Join-Path $BuildRoot 'libpsprecomp_core.a'
}
