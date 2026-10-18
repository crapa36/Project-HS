param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$Python,
    [Parameter(Mandatory = $true)][string]$FixtureScript,
    [Parameter(Mandatory = $true)][string]$GenerationVerifier,
    [Parameter(Mandatory = $true)][string]$Artifacts,
    [Parameter(Mandatory = $true)][string]$GenerationPointer
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# Process.CloseMainWindow ignores the hidden HWND used by this fixture.
# Match Runtime's exact main-window class and child PID, excluding console/auxiliary windows.
if (-not ('HsShaderReloadWindow' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
public static class HsShaderReloadWindow {
    private delegate bool WindowCallback(IntPtr window, IntPtr state);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumWindows(WindowCallback callback, IntPtr state);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int GetClassNameW(IntPtr window, StringBuilder className, int capacity);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)]
    private static extern bool PostMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    public static bool Close(uint processId) {
        bool posted = false;
        if (!EnumWindows((window, state) => {
            uint owner;
            if (GetWindowThreadProcessId(window, out owner) != 0 && owner == processId) {
                var className = new StringBuilder(256);
                if (GetClassNameW(window, className, className.Capacity) > 0 &&
                    className.ToString() == "ProjectHS.Window")
                    posted = PostMessageW(window, 0x0010, IntPtr.Zero, IntPtr.Zero) || posted;
            }
            return true;
        }, IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error());
        return posted;
    }
}
'@
}

$process = $null
$sourcePointerPath = $null
$sourcePointerBytes = $null
$fixtureRoot = $null
$logPath = Join-Path $Artifacts "shader_hot_reload.log"
$adapterPath = Join-Path $Artifacts "adapter.txt"
$renderPath = Join-Path $Artifacts "render.json"
$validationPath = Join-Path $Artifacts "d3d12_validation.log"
$stdoutPath = Join-Path $Artifacts "runtime.stdout.log"
$stderrPath = Join-Path $Artifacts "runtime.stderr.log"
$previousLocalAppData = $env:LOCALAPPDATA

function Read-ArtifactText([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() }
    finally { $reader.Dispose() }
}

function Read-ProcessDiagnostics {
    $stderr = (Read-ArtifactText $stderrPath).Trim()
    $stdout = (Read-ArtifactText $stdoutPath).Trim()
    if ($stderr.Length -gt 1200) {
        $stderr = $stderr.Substring(0, 1200) + "..."
    }
    if ($stdout.Length -gt 600) {
        $stdout = $stdout.Substring(0, 600) + "..."
    }
    if ([string]::IsNullOrWhiteSpace($stderr) -and [string]::IsNullOrWhiteSpace($stdout)) {
        return "stderr=<empty>; stdout=<empty>"
    }
    return "stderr=$stderr; stdout=$stdout"
}

function Wait-For([scriptblock]$Condition, [string]$Failure, [int]$TimeoutSeconds = 90) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($process -and $process.HasExited) {
            $process.WaitForExit()
            throw "$Failure Process exited with code $($process.ExitCode). $(Read-ProcessDiagnostics)"
        }
        if (& $Condition) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw $Failure
}

function Invoke-Fixture([string[]]$Arguments) {
    & $Python @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Shader reload fixture failed with exit code $LASTEXITCODE."
    }
}

function Stop-Gracefully([switch]$RequireSuccess) {
    if (-not $process -or $process.HasExited) {
        if ($process) { $process.WaitForExit() }
        if ($RequireSuccess -and $process -and $process.HasExited -and $process.ExitCode -ne 0) {
            throw "ProjectHS exited with code $($process.ExitCode)."
        }
        if ($process) {
            return $process.ExitCode
        }
        return 0
    }

    $closed = [HsShaderReloadWindow]::Close([uint32]$process.Id)
    if (-not $closed -and $RequireSuccess) {
        throw "ProjectHS did not accept WM_CLOSE for graceful shutdown."
    }
    if ($closed) {
        $deadline = [DateTime]::UtcNow.AddSeconds(15)
        while (-not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $process.HasExited) {
        if ($RequireSuccess) {
            throw "ProjectHS did not exit within 15 seconds after WM_CLOSE."
        }
        # A forced stop is only the cleanup fallback when WM_CLOSE did not work.
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
    }
    $process.WaitForExit()
    $exitCode = $process.ExitCode
    if ($null -eq $exitCode) {
        throw "ProjectHS exit code could not be read."
    }
    if ($RequireSuccess -and $exitCode -ne 0) {
        throw "ProjectHS exited with code $exitCode."
    }
    return $exitCode
}

try {
    $env:LOCALAPPDATA = Join-Path $Artifacts "ProfileRoot"
    New-Item -ItemType Directory -Force -Path $Artifacts | Out-Null
    Remove-Item -LiteralPath $logPath, $adapterPath, $renderPath, $validationPath,
        $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue

    $sourcePointerPath = (Resolve-Path -LiteralPath $GenerationPointer).Path
    $sourcePointerBytes = [System.IO.File]::ReadAllBytes($sourcePointerPath)
    $selectedBuildRoot = Split-Path -Parent (Split-Path -Parent $sourcePointerPath)
    # Leave room for hashed payloads and their temporary .pending suffix under MAX_PATH.
    $fixtureRoot = Join-Path (Split-Path -Parent $selectedBuildRoot) `
        (".r-" + [Guid]::NewGuid().ToString("N").Substring(0, 12))
    $fixtureArguments = @(
        $FixtureScript,
        "prepare",
        "--source-pointer", $sourcePointerPath,
        "--root", $fixtureRoot,
        "--generation-verifier", $GenerationVerifier,
        # This leaf belongs to the HLOD closure: a narrow reload must remain
        # supported even when that broad closure is no longer byte-identical.
        "--target-logical-id", "render/shader/leaf_0_scene.dxil",
        "--replacement-logical-id", "render/shader/leaf_1_scene.dxil"
    )
    Invoke-Fixture $fixtureArguments

    $fixtureMetadata = Get-Content -LiteralPath (Join-Path $fixtureRoot "fixture.json") -Raw |
        ConvertFrom-Json
    $baselineId = [string]$fixtureMetadata.baseline_id
    $invalidId = [string]$fixtureMetadata.invalid_id
    $validId = [string]$fixtureMetadata.valid_id
    $residentId = [string]$fixtureMetadata.resident_id
    $incompatibleId = [string]$fixtureMetadata.incompatible_id
    $activePointer = [string]$fixtureMetadata.active_pointer
    if ([string]::IsNullOrWhiteSpace($baselineId) -or
        [string]::IsNullOrWhiteSpace($invalidId) -or
        [string]::IsNullOrWhiteSpace($validId) -or
        [string]::IsNullOrWhiteSpace($residentId) -or
        [string]::IsNullOrWhiteSpace($incompatibleId) -or
        -not (Test-Path -LiteralPath $activePointer -PathType Leaf)) {
        throw "Shader reload fixture metadata is incomplete."
    }
    if (@($baselineId, $invalidId, $validId, $residentId, $incompatibleId | Select-Object -Unique).Count -ne 5) {
        throw "Shader reload fixture did not create distinct Generation IDs."
    }

    $processArguments = "--generation=`"$activePointer`" --artifacts=`"$Artifacts`" --no-vsync --frame-cap=60 --mute"
    $process = Start-Process -FilePath $Executable -ArgumentList $processArguments `
        -WindowStyle Hidden -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath -PassThru

    $escapedBaseline = [regex]::Escape($baselineId)
    # Cache the native handle while alive so Windows PowerShell retains ExitCode.
    $null = $process.Handle
    Wait-For {
        (Test-Path -LiteralPath $adapterPath -PathType Leaf) -and
        ((Read-ArtifactText $logPath) -match "ready active=$escapedBaseline(?:\r?\n|$)")
    } "Renderer did not initialize with the fixture baseline Generation."

    $selectArguments = @($FixtureScript, "select", "--root", $fixtureRoot, "--candidate", "invalid")
    Invoke-Fixture $selectArguments
    $escapedInvalid = [regex]::Escape($invalidId)
    Wait-For {
        $text = Read-ArtifactText $logPath
        $text -match "rejected candidate=$escapedInvalid active=$escapedBaseline stage=integrity .*previous content retained"
    } "Invalid shader Generation was not rejected during integrity verification."
    if ($process.HasExited) {
        throw "ProjectHS exited after rejecting the invalid shader Generation."
    }

    $escapedValid = [regex]::Escape($validId)
    $validManifest = Get-Content -LiteralPath (Join-Path $fixtureRoot "Generations/$validId/generation.json") -Raw | ConvertFrom-Json
    $targetProduct = @($validManifest.products | Where-Object { $_.logical_id -eq $fixtureMetadata.target_logical_id })
    if ($targetProduct.Count -ne 1) { throw "Valid fixture omits the replacement shader." }
    $pendingShader = [IO.Path]::GetFullPath((Join-Path $fixtureRoot $targetProduct[0].locator))
    $fixturePrefix = [IO.Path]::GetFullPath($fixtureRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (!$pendingShader.StartsWith($fixturePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Replacement shader escaped the isolated fixture."
    }
    $heldShader = $pendingShader + '.pending'
    Move-Item -LiteralPath $pendingShader -Destination $heldShader
    try {
        $selectArguments = @($FixtureScript, "select", "--root", $fixtureRoot, "--candidate", "valid")
        Invoke-Fixture $selectArguments
        $pointerBeforeRecovery = [IO.File]::ReadAllText($activePointer)
        Wait-For {
            (Read-ArtifactText $logPath) -match "rejected candidate=(?:$escapedValid|unresolved) active=$escapedBaseline stage=(?:manifest|integrity) .*previous content retained"
        } "Missing candidate shader was not rejected while preserving the baseline."
    }
    finally {
        if (Test-Path -LiteralPath $heldShader -PathType Leaf) {
            Move-Item -LiteralPath $heldShader -Destination $pendingShader
        }
    }
    if ([IO.File]::ReadAllText($activePointer) -cne $pointerBeforeRecovery) {
        throw "Recovery fixture unexpectedly republished the pointer."
    }
    Wait-For {
        $text = Read-ArtifactText $logPath
        $text -match "applied candidate=$escapedValid active=$escapedValid pipelines=1->2 parameters=1->1\b.*; temporal/HZB history invalidated"
    } "Recovered shader Generation was not adopted without republishing its pointer."
    if ($process.HasExited) {
        throw "ProjectHS exited after adopting the valid shader Generation."
    }
    Wait-For {
        (Read-ArtifactText $logPath) -match "rendered active=$escapedValid frame=[1-9][0-9]* pipelines=2 scene_generation=$escapedBaseline(?:\r?\n|$)"
    } "Adopted shader Generation did not render a frame with the retained resident scene."

    $escapedResident = [regex]::Escape($residentId)
    $escapedIncompatible = [regex]::Escape($incompatibleId)
    Invoke-Fixture @($FixtureScript, "select", "--root", $fixtureRoot, "--candidate", "incompatible")
    Wait-For {
        (Read-ArtifactText $logPath) -match "rejected candidate=$escapedIncompatible active=$escapedValid stage=scene_closure .*Scene dependency closure changed.*previous content retained"
    } "Changed arena layout was not rejected before resident adoption."

    Invoke-Fixture @($FixtureScript, "select", "--root", $fixtureRoot, "--candidate", "resident")
    Wait-For {
        (Read-ArtifactText $logPath) -match "applied candidate=$escapedResident active=$escapedResident pipelines=2->3 parameters=1->2\b.*vfx_changed=1\b"
    } "Compatible VFX catalog Generation was not adopted through the full resident path."
    Wait-For {
        (Read-ArtifactText $logPath) -match "rendered active=$escapedResident frame=[1-9][0-9]* pipelines=3 scene_generation=$escapedResident(?:\r?\n|$)"
    } "Full resident Generation did not render a frame with its prepared immutable scene."

    $exitCode = Stop-Gracefully -RequireSuccess
    $process = $null

    if (-not (Test-Path -LiteralPath $renderPath -PathType Leaf)) {
        throw "ProjectHS did not write render.json."
    }
    $render = Get-Content -LiteralPath $renderPath -Raw | ConvertFrom-Json
    if ([int]$render.validation_errors -ne 0) {
        throw "D3D12 validation reported $($render.validation_errors) errors."
    }
    $validationExists = Test-Path -LiteralPath $validationPath -PathType Leaf
    if ($validationExists -and (Get-Item -LiteralPath $validationPath).Length -ne 0) {
        throw "D3D12 validation log is not empty: $validationPath"
    }
}
finally {
    $env:LOCALAPPDATA = $previousLocalAppData
    if ($process) {
        Stop-Gracefully
    }
    if ($sourcePointerBytes -and $sourcePointerPath -and
        (Test-Path -LiteralPath $sourcePointerPath -PathType Leaf)) {
        $afterPointerBytes = [System.IO.File]::ReadAllBytes($sourcePointerPath)
        if (-not [System.Linq.Enumerable]::SequenceEqual($sourcePointerBytes, $afterPointerBytes)) {
            throw "Shader reload fixture changed the source Generation pointer."
        }
    }
}
