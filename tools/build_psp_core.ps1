[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Corpus,
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [Parameter(Mandatory = $true)][string]$Output,
    [switch]$Release,
    [switch]$Clean,
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
$PublicRoot = Split-Path -Parent $PSScriptRoot
$CorpusDir = [IO.Path]::GetFullPath($Corpus)
$Work = [IO.Path]::GetFullPath($WorkDir)
$Exe = [IO.Path]::GetFullPath($Output)
$PspRecompCore = Join-Path $PublicRoot 'tools\.build\psprecomp\libpsprecomp_core.a'
$NativeRoot = Join-Path $PublicRoot 'native'

foreach ($Needed in @($CorpusDir,$PspRecompCore,$NativeRoot)) {
    if (-not (Test-Path -LiteralPath $Needed)) { throw "Missing public build input: $Needed" }
}
if (-not (Get-ChildItem -LiteralPath $CorpusDir -File -Filter '*.cpp' -ErrorAction SilentlyContinue | Select-Object -First 1)) {
    throw "No generated PSP AOT units were found in $CorpusDir"
}

. (Join-Path $PSScriptRoot 'toolchain.ps1')
$Toolchain = Get-FucToolchain
$FfmpegRoot = & (Join-Path $PSScriptRoot 'ensure_ffmpeg.ps1') | Select-Object -Last 1
if (-not $FfmpegRoot -or -not (Test-Path -LiteralPath $FfmpegRoot -PathType Container)) {
    throw 'The verified FFmpeg LGPL dependency could not be prepared.'
}

if ($Clean -and (Test-Path -LiteralPath $Work)) { Remove-Item -LiteralPath $Work -Recurse -Force }
Reset-FucBuildDirectoryIfGeneratorChanged $Work $Toolchain.Generator
$Configuration = if ($Release) { 'Release' } else { 'Debug' }
$CmakeCorpusDir = $CorpusDir -replace '\\','/'
$CmakePspRecompCore = $PspRecompCore -replace '\\','/'
$CmakeFfmpegRoot = $FfmpegRoot -replace '\\','/'
& $Toolchain.Cmake -S $NativeRoot -B $Work -G $Toolchain.Generator `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DCMAKE_CXX_COMPILER=$($Toolchain.Compiler)" `
    "-DCMAKE_MAKE_PROGRAM=$($Toolchain.Make)" `
    "-DFUC_CORPUS_DIR=$CmakeCorpusDir" `
    "-DFUC_PSPRECOMP_CORE=$CmakePspRecompCore" `
    "-DFUC_FFMPEG_ROOT=$CmakeFfmpegRoot"
if ($LASTEXITCODE -ne 0) { throw 'Native PSP CMake configuration failed.' }

$BuildArguments = @('--build',$Work,'--parallel')
if ($Jobs -gt 0) { $BuildArguments += $Jobs }
& $Toolchain.Cmake @BuildArguments
if ($LASTEXITCODE -ne 0) { throw 'Native PSP compilation or link failed.' }

$BuiltExe = Join-Path $Work 'bin\FateUnlimitedCodesRecompCore.exe'
if (-not (Test-Path -LiteralPath $BuiltExe -PathType Leaf)) { throw "Native PSP executable was not created: $BuiltExe" }
$OutputDir = Split-Path -Parent $Exe
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
Copy-Item -LiteralPath $BuiltExe -Destination $Exe -Force

foreach ($Library in @('avcodec','avformat','avutil','swresample','swscale')) {
    $Dll = Get-ChildItem -LiteralPath (Join-Path $FfmpegRoot 'bin') -File -Filter "$Library-*.dll" | Select-Object -First 1
    if (-not $Dll) { throw "The FFmpeg runtime DLL for $Library is missing." }
    Copy-Item -LiteralPath $Dll.FullName -Destination $OutputDir -Force
}
$License = Get-ChildItem -LiteralPath $FfmpegRoot -File -Filter 'LICENSE*' | Select-Object -First 1
if ($License) { Copy-Item -LiteralPath $License.FullName -Destination (Join-Path $OutputDir 'FFmpeg-LICENSE.txt') -Force }

$Built = Get-Item -LiteralPath $Exe
[pscustomobject]@{
    Executable = $Exe
    SizeBytes = $Built.Length
    Configuration = $Configuration
    Compiler = $Toolchain.Compiler
}
