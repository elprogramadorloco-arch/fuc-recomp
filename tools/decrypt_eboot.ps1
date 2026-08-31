[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Iso,
    [Parameter(Mandatory)][string]$PpssppZip,
    [Parameter(Mandatory)][string]$OutputEboot
)

$ErrorActionPreference = 'Stop'
$PublicRoot = Split-Path -Parent $PSScriptRoot
$WorkRoot = Join-Path $PublicRoot 'tools\.build\ppsspp'
$RuntimeRoot = Join-Path $WorkRoot 'runtime'
$Memstick = Join-Path $WorkRoot 'memstick'
$AppendIni = Join-Path $WorkRoot 'dump_eboot.ini'
$ExpectedEbootSha256 = '33E8240519A232BE40BE503F17A23C2DFD5A361177FFB925DE444DF95DBE6BA7'
$ExpectedEbootSize = 2000320
$DumpTimeoutSeconds = 180

foreach ($InputFile in @($Iso, $PpssppZip)) {
    if (-not (Test-Path -LiteralPath $InputFile -PathType Leaf)) { throw "Missing input: $InputFile" }
}
if (Test-Path -LiteralPath $RuntimeRoot) {
    $ResolvedRuntime = [IO.Path]::GetFullPath($RuntimeRoot)
    $ResolvedWork = [IO.Path]::GetFullPath($WorkRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
    if (-not $ResolvedRuntime.StartsWith(
            $ResolvedWork + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe PPSSPP runtime cleanup path.' }
    Remove-Item -LiteralPath $ResolvedRuntime -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $RuntimeRoot,$Memstick,(Split-Path -Parent $OutputEboot) | Out-Null
Expand-Archive -LiteralPath $PpssppZip -DestinationPath $RuntimeRoot -Force
$Ppsspp = Get-ChildItem -LiteralPath $RuntimeRoot -Recurse -File -Filter 'PPSSPPWindows64.exe' | Select-Object -First 1
if (-not $Ppsspp) { throw 'PPSSPPWindows64.exe was not found in ppsspp.zip.' }

@'
[General]
DumpFileTypes = 1
'@ | Set-Content -LiteralPath $AppendIni -Encoding ascii

$DumpRoots = @($Memstick, (Join-Path $RuntimeRoot 'memstick')) | Select-Object -Unique
foreach ($Root in $DumpRoots) {
    if (Test-Path -LiteralPath $Root -PathType Container) {
        Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '(?i)eboot' -and $_.Extension -ieq '.bin' } |
            Remove-Item -Force
    }
}

function Find-CompleteEbootDump {
    $Roots = @($DumpRoots | Where-Object { Test-Path -LiteralPath $_ -PathType Container })
    if (-not $Roots) { return $null }
    foreach ($Candidate in @(Get-ChildItem -LiteralPath $Roots -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '(?i)eboot' -and $_.Extension -ieq '.bin' } |
            Sort-Object LastWriteTimeUtc -Descending)) {
        if ($Candidate.Length -ne $ExpectedEbootSize) { continue }
        try {
            if ((Get-FileHash -Algorithm SHA256 -LiteralPath $Candidate.FullName).Hash -eq $ExpectedEbootSha256) {
                return $Candidate
            }
        } catch { }
    }
    return $null
}

Write-Host 'PPSSPP is using an isolated memstick and its official decrypted-EBOOT dump option.'
Write-Host 'The dump will be detected automatically; no menu input or manual close is required.'
$Arguments = @(
    '--windowed',
    '--pause-menu-exit',
    "--memstick=`"$Memstick`"",
    "--appendconfig=`"$AppendIni`"",
    "`"$Iso`""
)
$Process = Start-Process -FilePath $Ppsspp.FullName -WorkingDirectory $Ppsspp.DirectoryName -ArgumentList $Arguments -PassThru
$Deadline = [DateTime]::UtcNow.AddSeconds($DumpTimeoutSeconds)
$Dump = $null
try {
    while ([DateTime]::UtcNow -lt $Deadline) {
        $Dump = Find-CompleteEbootDump
        if ($Dump) { break }
        $Process.Refresh()
        if ($Process.HasExited) { break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $Dump) {
        throw "PPSSPP did not produce the verified decrypted EBOOT within $DumpTimeoutSeconds seconds."
    }
} finally {
    $Process.Refresh()
    if (-not $Process.HasExited) {
        $null = $Process.CloseMainWindow()
        if (-not $Process.WaitForExit(5000)) {
            Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
            $Process.WaitForExit()
        }
    }
}
Copy-Item -LiteralPath $Dump.FullName -Destination $OutputEboot -Force
Write-Host "Decrypted EBOOT created locally: $OutputEboot"
