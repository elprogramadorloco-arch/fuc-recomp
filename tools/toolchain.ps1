$ErrorActionPreference = 'Stop'

function Get-FucCmake {
    $Candidates = @(
        'C:\Program Files\CMake\bin\cmake.exe',
        (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -Unique
    $Cmake = $Candidates | Select-Object -First 1
    if (-not $Cmake) { throw 'CMake 3.22 or newer was not found. Install CMake for Windows and add it to PATH.' }
    $VersionLine = (& $Cmake --version | Select-Object -First 1)
    $Version = if ($VersionLine -match '(\d+\.\d+(?:\.\d+)*)') { [version]$Matches[1] } else { $null }
    if (-not $Version -or $Version -lt [version]'3.22') {
        throw "CMake 3.22 or newer is required; detected: $VersionLine"
    }
    [pscustomobject]@{ Path = $Cmake; Version = $Version }
}

function Get-FucToolchain {
    $Cmake = Get-FucCmake
    $Root = & (Join-Path $PSScriptRoot 'ensure_toolchain.ps1') | Select-Object -Last 1
    if (-not $Root -or -not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw 'The verified portable LLVM-MinGW toolchain could not be prepared.'
    }
    [pscustomobject]@{
        Cmake = $Cmake.Path
        CmakeVersion = $Cmake.Version
        Root = $Root
        Compiler = Join-Path $Root 'bin\clang++.exe'
        Make = Join-Path $Root 'bin\mingw32-make.exe'
        Generator = 'MinGW Makefiles'
    }
}

function Reset-FucBuildDirectoryIfGeneratorChanged([string]$BuildDirectory, [string]$ExpectedGenerator = 'MinGW Makefiles') {
    $Cache = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $Cache -PathType Leaf)) { return }
    $GeneratorLine = Get-Content -LiteralPath $Cache | Where-Object { $_ -like 'CMAKE_GENERATOR:INTERNAL=*' } | Select-Object -First 1
    if ($GeneratorLine -ne "CMAKE_GENERATOR:INTERNAL=$ExpectedGenerator") {
        $PublicRoot = Split-Path -Parent $PSScriptRoot
        $AllowedRoot = [IO.Path]::GetFullPath((Join-Path $PublicRoot 'tools\.build')).TrimEnd('\') + '\'
        $Resolved = [IO.Path]::GetFullPath($BuildDirectory)
        if (-not $Resolved.StartsWith($AllowedRoot,[StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to reset a build directory outside tools\.build: $Resolved"
        }
        Remove-Item -LiteralPath $Resolved -Recurse -Force
    }
}
