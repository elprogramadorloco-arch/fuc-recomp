[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallRoot,
    [Parameter(Mandatory)][string]$UpdateRoot
)

$ErrorActionPreference = 'SilentlyContinue'
Start-Sleep -Seconds 2
$InstallRoot = [IO.Path]::GetFullPath($InstallRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
$UpdateRoot = [IO.Path]::GetFullPath($UpdateRoot)
$Expected = Join-Path $InstallRoot 'update'
if ($UpdateRoot -eq $Expected -and $UpdateRoot.StartsWith(
        $InstallRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase) -and
    (Test-Path -LiteralPath $UpdateRoot)) {
    Remove-Item -LiteralPath $UpdateRoot -Recurse -Force
}
