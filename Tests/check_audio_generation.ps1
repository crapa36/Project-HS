[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GenerationExecutable,
    [Parameter(Mandatory = $true)][string]$BuildRoot,
    [Parameter(Mandatory = $true)][string]$SourceRoot
)

$ErrorActionPreference = 'Stop'
$buildRootFull = [IO.Path]::GetFullPath($BuildRoot)
$sourceRootFull = [IO.Path]::GetFullPath($SourceRoot)
$expectedSource = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (![StringComparer]::OrdinalIgnoreCase.Equals($sourceRootFull, $expectedSource) -or
    !$buildRootFull.StartsWith($sourceRootFull + [IO.Path]::DirectorySeparatorChar,
                               [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Audio Generation check requires this repository and its contained build directory.'
}

if (!(Test-Path -LiteralPath $GenerationExecutable -PathType Leaf)) {
    throw "Generation executable does not exist: $GenerationExecutable"
}

$ErrorActionPreference = 'Continue'
$output = (& $GenerationExecutable --verify --build-root $buildRootFull --scope test-audio 2>&1 |
    ForEach-Object { $_.ToString() } | Out-String).Trim()
$exitCode = $LASTEXITCODE
Write-Host $output
exit $exitCode
