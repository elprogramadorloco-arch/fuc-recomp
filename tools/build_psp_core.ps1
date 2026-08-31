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
$PspRecompCore = Join-Path $PublicRoot 'tools\.build\psprecomp\Release\psprecomp_core.lib'
$NativeRoot = Join-Path $PublicRoot 'native'

foreach ($Needed in @($CorpusDir,$PspRecompCore,$NativeRoot)) {
    if (-not (Test-Path -LiteralPath $Needed)) { throw "Missing public build input: $Needed" }
}

$FfmpegRoot = & (Join-Path $PSScriptRoot 'ensure_ffmpeg.ps1') | Select-Object -Last 1
if (-not $FfmpegRoot -or -not (Test-Path -LiteralPath $FfmpegRoot)) {
    throw 'The verified FFmpeg LGPL dependency could not be prepared.'
}

$env:NoDefaultCurrentDirectoryInExePath = $null
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $VsWhere)) { throw "vswhere not found at $VsWhere" }
$VsPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $VsPath) { throw 'No Visual Studio or Build Tools installation with the MSVC x64 toolset was found.' }
$VcVars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'

if ($Clean -and (Test-Path -LiteralPath $Work)) { Remove-Item -LiteralPath $Work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Work | Out-Null

$Includes = @(
    "/I`"$(Join-Path $PublicRoot 'PSPRecomp\include')`"",
    "/I`"$(Join-Path $NativeRoot 'include')`"",
    "/I`"$(Join-Path $NativeRoot 'recomp')`"",
    "/I`"$(Join-Path $NativeRoot 'media')`"",
    "/I`"$(Join-Path $FfmpegRoot 'include')`""
) -join ' '
$ConfigurationFlags = if ($Release) { '/O2 /Ob2 /MD /DNDEBUG' } else { '/Od /MDd' }
$Common = "/nologo /std:c++20 /EHsc $ConfigurationFlags /Zc:preprocessor /D_CRT_SECURE_NO_WARNINGS $Includes"
$Common += if ($Jobs -gt 0) { " /MP$Jobs" } else { ' /MP' }

$Sources = @()
$Sources += Get-ChildItem -LiteralPath $CorpusDir -File -Filter '*.cpp' | Sort-Object Name | ForEach-Object FullName
$Sources += @(
    (Join-Path $NativeRoot 'recomp\fate_psp_profile.cpp'),
    (Join-Path $NativeRoot 'recomp\psp_entry_host.cpp'),
    (Join-Path $NativeRoot 'src\port\player_input.cpp'),
    (Join-Path $NativeRoot 'src\port\psp_pad.cpp'),
    (Join-Path $NativeRoot 'src\port\state_bridge.cpp'),
    (Join-Path $NativeRoot 'src\port\fate_psp_battle_map.cpp'),
    (Join-Path $NativeRoot 'src\port\psp_atrac.cpp'),
    (Join-Path $NativeRoot 'src\port\psp_mpeg.cpp'),
    (Join-Path $NativeRoot 'src\port\psp_pgf.cpp'),
    (Join-Path $NativeRoot 'src\port\psp_vag.cpp'),
    (Join-Path $NativeRoot 'src\renderer\psp_ge_command.cpp'),
    (Join-Path $NativeRoot 'src\renderer\psp_ge_surface.cpp'),
    (Join-Path $NativeRoot 'src\renderer\psp_ge_d3d11_surface.cpp'),
    (Join-Path $NativeRoot 'src\platform\windows_psp_audio.cpp'),
    (Join-Path $NativeRoot 'src\platform\windows_psp_window.cpp'),
    (Join-Path $NativeRoot 'media\vcs_media_decoder.cpp')
)
foreach ($Source in $Sources) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Missing native PSP source: $Source" }
}

$HeaderStamp = (Get-ChildItem -File -Recurse -Include *.h,*.hpp -Path `
    (Join-Path $PublicRoot 'PSPRecomp\include'),(Join-Path $NativeRoot 'include'),
    (Join-Path $NativeRoot 'recomp'),(Join-Path $NativeRoot 'media') |
    Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum
$Stale = @()
foreach ($Source in $Sources) {
    $Object = Join-Path $Work ([IO.Path]::GetFileNameWithoutExtension($Source) + '.obj')
    if (-not (Test-Path -LiteralPath $Object)) { $Stale += $Source; continue }
    $ObjectTime = (Get-Item -LiteralPath $Object).LastWriteTimeUtc
    if ((Get-Item -LiteralPath $Source).LastWriteTimeUtc -gt $ObjectTime -or $HeaderStamp -gt $ObjectTime) { $Stale += $Source }
}
Write-Output "Sources: $($Sources.Count); recompiling: $($Stale.Count)"

if ($Stale.Count -gt 0) {
    $SourceList = Join-Path $Work 'sources.rsp'
    ($Stale | ForEach-Object { '"' + $_ + '"' }) -join "`r`n" | Set-Content -LiteralPath $SourceList -Encoding utf8
    # A trailing backslash immediately before a closing quote is interpreted
    # as an escaped quote by the MSVC command-line parser. Use a forward slash
    # for the /Fo directory terminator so the following @response file remains
    # a separate argument even when the installation path contains spaces.
    $ObjectDirectoryArgument = ($Work -replace '\\','/') + '/'
    $Compile = "`"$VcVars`" >nul 2>&1 && cl $Common /c /Fo`"$ObjectDirectoryArgument`" @`"$SourceList`""
    $CompileOutput = cmd /c $Compile 2>&1
    $CompileErrors = $CompileOutput | Select-String -Pattern 'error [A-Z]+[0-9]+' | Select-Object -First 20
    if ($CompileErrors) {
        $CompileErrors | ForEach-Object { Write-Output $_.Line }
        throw 'Native PSP compilation failed.'
    }
}

$Objects = Get-ChildItem -LiteralPath $Work -File -Filter '*.obj' | ForEach-Object { '"' + $_.FullName + '"' }
$ObjectList = Join-Path $Work 'objects.rsp'
(($Objects -join "`r`n") + "`r`n`"$PspRecompCore`"") | Set-Content -LiteralPath $ObjectList -Encoding utf8
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Exe) | Out-Null
$LinkFlags = if ($Release) { '/OPT:REF /OPT:ICF' } else { '' }
$FfmpegLibs = @('avcodec','avformat','avutil','swresample','swscale') |
    ForEach-Object { '"' + (Join-Path $FfmpegRoot "lib\$_.lib") + '"' }
$Link = "`"$VcVars`" >nul 2>&1 && link /nologo $LinkFlags /STACK:268435456 /OUT:`"$Exe`" @`"$ObjectList`" user32.lib gdi32.lib d3d11.lib d3dcompiler.lib xinput9_1_0.lib winmm.lib $($FfmpegLibs -join ' ')"
$LinkOutput = cmd /c $Link 2>&1
$LinkErrors = $LinkOutput | Select-String -Pattern 'error LNK' | Select-Object -First 20
if ($LinkErrors) {
    $LinkErrors | ForEach-Object { Write-Output $_.Line }
    throw 'Native PSP link failed.'
}

$OutputDir = Split-Path -Parent $Exe
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
    Objects = $Objects.Count
    Recompiled = $Stale.Count
    Configuration = if ($Release) { 'Release' } else { 'Debug' }
}
