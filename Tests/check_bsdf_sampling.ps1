param(
    [Parameter(Mandatory = $true)][string]$Runner,
    [Parameter(Mandatory = $true)][string]$GenerationPointer,
    [Parameter(Mandatory = $true)][string]$ProbeShader
)
$ErrorActionPreference = 'Stop'
$pointerPath = (Resolve-Path -LiteralPath $GenerationPointer).Path
$buildRoot = Split-Path -Parent (Split-Path -Parent $pointerPath)
$pointer = Get-Content -LiteralPath $pointerPath -Raw | ConvertFrom-Json
$manifest = Get-Content -LiteralPath (Join-Path $buildRoot $pointer.manifest_locator) -Raw |
    ConvertFrom-Json
$shader = $null
$coatedDfg = $null
foreach ($logicalId in @('render/shader/bsdf_preintegrate_cs.dxil', 'render/shader/deferred_ps.dxil',
        'render/lighting/coated_ggx_dfg.dds')) {
    $products = @($manifest.products | Where-Object logical_id -eq $logicalId)
    if ($products.Count -ne 1) { throw "Selected Generation must contain exactly one $logicalId." }
    $path = Join-Path $buildRoot $products[0].locator
    $stream = [System.IO.File]::OpenRead($path)
    $hash = [System.Security.Cryptography.SHA256]::Create()
    try {
        $digest = 'sha256:' + [System.BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
        if ($stream.Length -ne $products[0].size -or $digest -ne $products[0].digest) {
            throw "Selected $logicalId does not match its manifest digest and size."
        }
    } finally {
        $hash.Dispose()
        $stream.Dispose()
    }
    if ($logicalId -eq 'render/shader/bsdf_preintegrate_cs.dxil') { $shader = $path }
    if ($logicalId -eq 'render/lighting/coated_ggx_dfg.dds') { $coatedDfg = $path }
}
& $Runner $shader $ProbeShader $coatedDfg
if ($LASTEXITCODE -ne 0) { throw "GPU BSDF sampling verification failed: $LASTEXITCODE" }
