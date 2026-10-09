param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$Artifacts,
    [Parameter(Mandatory = $true)]
    [string]$GenerationPointer,
    [string]$BaselineRoot,
    [string]$BaselineDirectory
)

$ErrorActionPreference = 'Stop'
$ensure = Join-Path $PSScriptRoot 'ensure_render_baseline.ps1'
if (!$BaselineRoot) {
    $BaselineRoot = if ($BaselineDirectory) {
        Split-Path ([IO.Path]::GetFullPath($BaselineDirectory)) -Parent
    } else {
        Join-Path $Artifacts 'render-baseline'
    }
}
$ensureArgs = @{
    Executable = $Executable
    GenerationPointer = $GenerationPointer
    ArtifactsRoot = $BaselineRoot
}
if ($BaselineDirectory) { $ensureArgs.BaselineDirectory = $BaselineDirectory }
$legacy = @(& $ensure @ensureArgs) | Select-Object -Last 1
if (!$legacy -or !(Test-Path -LiteralPath $legacy -PathType Container)) {
    throw 'Legacy render baseline is unavailable.'
}
$enhanced = Join-Path $Artifacts "enhanced"
$options = Join-Path $Artifacts "options"
$generationArgument = "--generation=$GenerationPointer"

function Test-RenderArtifacts([string]$Directory, [int]$ParticleCapacity,
                              [bool]$Bloom = $true, [bool]$Outline = $true) {
    $required = @(
        "result.json",
        "spec.json",
        "timeline.csv",
        "gpu_passes.csv",
        "events.ndjson",
        "render.json",
        "dred_config.json",
        "particle_stats.json",
        "renderer_features.jsonl",
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
    $result = Get-Content (Join-Path $Directory "result.json") -Raw | ConvertFrom-Json
    if (-not $result.execution_valid -or -not $result.assertions_passed -or
        [int]$result.tick -ne 180 -or [int]$result.rendered_frames -ne 180) {
        throw "Render smoke result is invalid or incomplete in $Directory."
    }
    $render = Get-Content (Join-Path $Directory "render.json") -Raw | ConvertFrom-Json
    if ([int]$render.last_tick -ne 180 -or [int]$render.validation_errors -ne 0) {
        throw "Render validation or timing contract failed in $Directory."
    }
    $gpuRows = @(Import-Csv (Join-Path $Directory "gpu_passes.csv"))
    $uploadRows = @(Import-Csv (Join-Path $Directory "dynamic_instance_upload.csv"))
    $lastUpload = $uploadRows | Select-Object -Last 1
    if (!$lastUpload -or [int]$lastUpload.frame -ne [int]$gpuRows[0].frame) {
        throw "GPU pass timestamps do not match the scene upload frame."
    }
    if (!$lastUpload.PSObject.Properties['stable_slots'] -or
        !$lastUpload.PSObject.Properties['anonymous_instances']) {
        throw "Persistent GPU scene slot diagnostics are missing."
    }
    if (@($uploadRows | Where-Object { [int]$_.anonymous_instances -ne 0 }).Count -ne 0) {
        throw "Render smoke produced anonymous persistent GPU scene instances."
    }
    if ($uploadRows.Count -gt 1 -and
        @($uploadRows | Select-Object -Skip 1 | Where-Object {
            [int]$_.uploaded_bytes -eq 0 -and [int]$_.copy_regions -eq 0
        }).Count -eq 0) {
        throw "Persistent GPU scene never reached an unchanged zero-upload frame."
    }
    $hasSceneUpload = @($gpuRows | Where-Object { $_.pass -eq 'Scene Buffer Upload' }).Count -eq 1
    if ([int]$lastUpload.uploaded_bytes -gt 0 -and !$hasSceneUpload) {
        throw "Scene Buffer Upload timestamp is missing for copied scene bytes."
    }
    $basePasses = @(
        'GPU Particle Spawn/Update', '3-cascade Directional Shadow',
        'Static Visibility', 'GBuffer+Depth', 'Deferred Cel Lighting',
        'VFX Distortion Vectors', 'Forward Transparent/OIT', 'OIT Composite',
        'Temporal Resolve', 'ToneMap', 'FXAA', 'Game UI', 'Depth Pyramid',
        'Visibility Retest', 'GBuffer Retest', 'Final Depth Pyramid', 'UI Upload'
    )
    $actualPasses = @($gpuRows | ForEach-Object { $_.pass })
    if (@($actualPasses | Sort-Object -Unique).Count -ne $actualPasses.Count) {
        throw 'GPU pass timestamps contain duplicate pass names.'
    }
    $missing = @($basePasses | Where-Object { $_ -cnotin $actualPasses })
    if ($missing.Count -ne 0) {
        throw "GPU base pass contract mismatch: missing=$($missing -join ', ')."
    }
    if ([int]$lastUpload.skin_gpu_dispatches -gt 0 -and
        'Skin Cache' -cnotin $actualPasses) {
        throw 'Skin Cache dispatches were recorded without a Skin Cache GPU pass.'
    }
    $bloomPasses = @('Bloom Half Extract', 'Bloom Quarter Downsample',
                     'Bloom Half Upsample', 'Bloom')
    $missingBloom = @($bloomPasses | Where-Object { $_ -cnotin $actualPasses })
    $presentBloom = @($bloomPasses | Where-Object { $_ -cin $actualPasses })
    if ($Bloom -and $missingBloom.Count -ne 0) {
        throw "Bloom is enabled but required passes are missing: $($missingBloom -join ', ')."
    }
    if (!$Bloom -and $presentBloom.Count -ne 0) {
        throw "Bloom is disabled but passes were emitted: $($presentBloom -join ', ')."
    }
    $outlinePass = 'Screen-space Outline'
    $presentOutline = @($actualPasses | Where-Object { $_ -ceq $outlinePass })
    if ($Outline -and $presentOutline.Count -ne 1) {
        throw 'Outline is enabled but Screen-space Outline was not emitted exactly once.'
    }
    if (!$Outline -and $presentOutline.Count -ne 0) {
        throw 'Outline is disabled but Screen-space Outline was emitted.'
    }
    $particleStats = Get-Content (Join-Path $Directory "particle_stats.json") -Raw |
        ConvertFrom-Json
    if ($particleStats.alive -gt $ParticleCapacity -or
        $particleStats.capacity -ne $ParticleCapacity) {
        throw "GPU particle lifecycle mismatch: alive=$($particleStats.alive), capacity=$($particleStats.capacity)."
    }
}

function Test-LockstepTimeline([string]$Directory) {
    $rows = @(Import-Csv (Join-Path $Directory "timeline.csv"))
    if ($rows.Count -ne 180) {
        throw "Expected one rendered frame per smoke tick in $Directory, got $($rows.Count)."
    }
    for ($index = 0; $index -lt $rows.Count; ++$index) {
        $expected = $index + 1
        if ([int]$rows[$index].frame -ne $expected -or [int]$rows[$index].tick -ne $expected) {
            throw "Smoke render lockstep diverged in $Directory at row $expected."
        }
    }
}

& $Executable --smoke --smoke-render-lockstep --ticks=180 --barriers=enhanced --resize-test `
    $generationArgument --artifacts=$enhanced
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
Test-LockstepTimeline $legacy
Test-LockstepTimeline $enhanced

Add-Type -AssemblyName System.Drawing
if (-not ("BarrierCaptureComparer" -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class BarrierCaptureComparer
{
    public static int[] Compare(Bitmap left, Bitmap right)
    {
        var bounds = new Rectangle(0, 0, left.Width, left.Height);
        using (var leftArgb = left.Clone(bounds, PixelFormat.Format32bppArgb))
        using (var rightArgb = right.Clone(bounds, PixelFormat.Format32bppArgb))
        {
            BitmapData leftData = null;
            BitmapData rightData = null;
            try
            {
                leftData = leftArgb.LockBits(bounds, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                rightData = rightArgb.LockBits(bounds, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                var rowBytes = left.Width * 4;
                var leftRow = new byte[rowBytes];
                var rightRow = new byte[rowBytes];
                var changedPixels = 0;
                var maximumDifference = 0;
                for (var y = 0; y < left.Height; ++y)
                {
                    Marshal.Copy(IntPtr.Add(leftData.Scan0, y * leftData.Stride), leftRow, 0, rowBytes);
                    Marshal.Copy(IntPtr.Add(rightData.Scan0, y * rightData.Stride), rightRow, 0, rowBytes);
                    for (var offset = 0; offset < rowBytes; offset += 4)
                    {
                        var difference = Math.Max(
                            Math.Max(Math.Abs(leftRow[offset] - rightRow[offset]), Math.Abs(leftRow[offset + 1] - rightRow[offset + 1])),
                            Math.Max(Math.Abs(leftRow[offset + 2] - rightRow[offset + 2]), Math.Abs(leftRow[offset + 3] - rightRow[offset + 3])));
                        if (difference != 0)
                        {
                            ++changedPixels;
                            maximumDifference = Math.Max(maximumDifference, difference);
                        }
                    }
                }
                return new[] { changedPixels, maximumDifference };
            }
            finally
            {
                if (leftData != null)
                {
                    leftArgb.UnlockBits(leftData);
                }
                if (rightData != null)
                {
                    rightArgb.UnlockBits(rightData);
                }
            }
        }
    }
}
'@
}
$legacyPng = [System.Drawing.Bitmap]::new((Join-Path $legacy "capture_tick_180.png"))
$enhancedPng = [System.Drawing.Bitmap]::new((Join-Path $enhanced "capture_tick_180.png"))
try {
    if ($legacyPng.Size -ne $enhancedPng.Size) {
        throw "Rendered capture dimensions differ between barrier paths."
    }
    $changedPixels = 0
    $maximumDifference = 0
    $comparison = [BarrierCaptureComparer]::Compare($legacyPng, $enhancedPng)
    $changedPixels = $comparison[0]
    $maximumDifference = $comparison[1]
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

function Get-NormalizedPassNames([string]$Directory) {
    return @(Import-Csv (Join-Path $Directory "gpu_passes.csv") |
        ForEach-Object { ([string]$_.pass).Trim() } |
        Sort-Object -Unique)
}

$legacyPasses = Get-NormalizedPassNames $legacy
$enhancedPasses = Get-NormalizedPassNames $enhanced
$legacyUpload = @(Import-Csv (Join-Path $legacy "dynamic_instance_upload.csv")) | Select-Object -Last 1
$enhancedUpload = @(Import-Csv (Join-Path $enhanced "dynamic_instance_upload.csv")) | Select-Object -Last 1
$legacyOnly = @($legacyPasses | Where-Object { $_ -cnotin $enhancedPasses })
$enhancedOnly = @($enhancedPasses | Where-Object { $_ -cnotin $legacyPasses })
$sceneUploadPass = 'Scene Buffer Upload'
if ($legacyOnly -contains $sceneUploadPass -and
    $enhancedOnly.Count -eq 0 -and [int]$enhancedUpload.uploaded_bytes -eq 0) {
    $legacyOnly = @($legacyOnly | Where-Object { $_ -cne $sceneUploadPass })
}
elseif ($enhancedOnly -contains $sceneUploadPass -and
        $legacyOnly.Count -eq 0 -and [int]$legacyUpload.uploaded_bytes -eq 0) {
    $enhancedOnly = @($enhancedOnly | Where-Object { $_ -cne $sceneUploadPass })
}
if ($legacyOnly.Count -ne 0 -or $enhancedOnly.Count -ne 0) {
    throw "Legacy and enhanced GPU pass sets differ: legacy_only=$($legacyOnly -join ', '); enhanced_only=$($enhancedOnly -join ', ')."
}

& $Executable --smoke --smoke-render-lockstep --ticks=180 --barriers=enhanced --resize-test `
    --render-scale=75 --shadow=2048 --particles=50 --no-bloom --no-outline `
    $generationArgument --artifacts=$options
if ($LASTEXITCODE -ne 0) {
    throw "Stage 1 options smoke failed."
}
Test-RenderArtifacts $options 5000 $false $false
Test-LockstepTimeline $options
