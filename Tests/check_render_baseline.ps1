param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$GenerationPointer,
    [Parameter(Mandatory = $true)][string]$ArtifactsRoot
)

$ErrorActionPreference = 'Stop'
$ensure = Join-Path $PSScriptRoot 'ensure_render_baseline.ps1'
$baseline = @(& $ensure -Executable $Executable -GenerationPointer $GenerationPointer -ArtifactsRoot $ArtifactsRoot) |
    Select-Object -Last 1
if (!$baseline -or !(Test-Path -LiteralPath $baseline -PathType Container)) {
    throw 'Render baseline capture did not publish an artifact directory.'
}
$result = Get-Content -LiteralPath (Join-Path $baseline 'result.json') -Raw |
    ConvertFrom-Json
if (!$result.execution_valid -or !$result.assertions_passed -or
    [int]$result.tick -ne 180 -or [int]$result.rendered_frames -ne 180) {
    throw 'Full runtime render smoke assertions failed.'
}
Write-Output "integration.render-smoke baseline=$baseline checksum=$($result.checksum) frames=180"
