param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$Artifacts
)

$legacy = Join-Path $Artifacts "legacy"
$enhanced = Join-Path $Artifacts "enhanced"
$options = Join-Path $Artifacts "options"

function Test-RenderArtifacts([string]$Directory, [int]$ParticleCapacity) {
    $required = @(
        "result.json",
        "spec.json",
        "timeline.csv",
        "gpu_passes.csv",
        "events.ndjson",
        "render.json",
        "dred_config.json",
        "particle_stats.json",
        "capture_tick_180.png"
    )
    foreach ($name in $required) {
        if (-not (Test-Path (Join-Path $Directory $name))) {
            throw "Missing Stage 1 artifact: $Directory/$name"
        }
    }
    $dred = Get-Content (Join-Path $Directory "dred_config.json") -Raw | ConvertFrom-Json
    if (-not $dred.enabled -or -not $dred.auto_breadcrumbs -or -not $dred.page_fault) {
        throw "DRED was not enabled before device creation."
    }
    $gpuRows = Import-Csv (Join-Path $Directory "gpu_passes.csv")
    if ($gpuRows.Count -ne 11) {
        throw "Expected 11 GPU pass timestamps, got $($gpuRows.Count)."
    }
    $particleStats = Get-Content (Join-Path $Directory "particle_stats.json") -Raw |
        ConvertFrom-Json
    if ($particleStats.alive -gt $ParticleCapacity -or
        $particleStats.capacity -ne $ParticleCapacity) {
        throw "GPU particle lifecycle mismatch: alive=$($particleStats.alive), capacity=$($particleStats.capacity)."
    }
}

& $Executable --smoke --ticks=180 --barriers=legacy --artifacts=$legacy
if ($LASTEXITCODE -ne 0) {
    throw "Legacy barrier smoke failed."
}

& $Executable --smoke --ticks=180 --barriers=enhanced --artifacts=$enhanced
if ($LASTEXITCODE -ne 0) {
    throw "Enhanced barrier smoke failed."
}

$legacyResult = Get-Content (Join-Path $legacy "result.json") -Raw | ConvertFrom-Json
$enhancedResult = Get-Content (Join-Path $enhanced "result.json") -Raw | ConvertFrom-Json
if (-not $legacyResult.execution_valid -or -not $enhancedResult.execution_valid) {
    throw "A barrier smoke result is invalid."
}
if ($legacyResult.checksum -ne $enhancedResult.checksum) {
    throw "Gameplay checksums differ between barrier paths."
}

Add-Type -AssemblyName System.Drawing
$legacyPng = [System.Drawing.Bitmap]::new((Join-Path $legacy "capture_tick_180.png"))
$enhancedPng = [System.Drawing.Bitmap]::new((Join-Path $enhanced "capture_tick_180.png"))
try {
    if ($legacyPng.Size -ne $enhancedPng.Size) {
        throw "Rendered capture dimensions differ between barrier paths."
    }
    $changedPixels = 0
    $maximumDifference = 0
    for ($y = 0; $y -lt $legacyPng.Height; ++$y) {
        for ($x = 0; $x -lt $legacyPng.Width; ++$x) {
            $left = $legacyPng.GetPixel($x, $y)
            $right = $enhancedPng.GetPixel($x, $y)
            $difference = [Math]::Max(
                [Math]::Max([Math]::Abs($left.R - $right.R), [Math]::Abs($left.G - $right.G)),
                [Math]::Max([Math]::Abs($left.B - $right.B), [Math]::Abs($left.A - $right.A))
            )
            if ($difference -ne 0) {
                ++$changedPixels
                $maximumDifference = [Math]::Max($maximumDifference, $difference)
            }
        }
    }
    $allowedChangedPixels = [Math]::Ceiling($legacyPng.Width * $legacyPng.Height * 0.0001)
    if ($maximumDifference -gt 1 -or $changedPixels -gt $allowedChangedPixels) {
        throw "Barrier captures exceed tolerance: changed=$changedPixels, max=$maximumDifference."
    }
}
finally {
    $legacyPng.Dispose()
    $enhancedPng.Dispose()
}
Test-RenderArtifacts $legacy 10000
Test-RenderArtifacts $enhanced 10000

& $Executable --smoke --ticks=180 --barriers=enhanced --resize-test `
    --render-scale=75 --shadow=2048 --particles=50 --no-bloom --no-outline `
    --artifacts=$options
if ($LASTEXITCODE -ne 0) {
    throw "Stage 1 options smoke failed."
}
Test-RenderArtifacts $options 5000
