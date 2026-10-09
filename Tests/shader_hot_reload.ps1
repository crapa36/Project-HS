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

    $closed = $process.CloseMainWindow()
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
    New-Item -ItemType Directory -Force -Path $Artifacts | Out-Null
    Remove-Item -LiteralPath $logPath, $adapterPath, $renderPath, $validationPath,
        $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue

    $sourcePointerPath = (Resolve-Path -LiteralPath $GenerationPointer).Path
    $sourcePointerBytes = [System.IO.File]::ReadAllBytes($sourcePointerPath)
    $selectedBuildRoot = Split-Path -Parent (Split-Path -Parent $sourcePointerPath)
    $fixtureRoot = Join-Path (Split-Path -Parent $selectedBuildRoot) `
        (".reload-" + [Guid]::NewGuid().ToString("N"))
    $fixtureArguments = @(
        $FixtureScript,
        "prepare",
        "--source-pointer", $sourcePointerPath,
        "--root", $fixtureRoot,
        "--generation-verifier", $GenerationVerifier
    )
    Invoke-Fixture $fixtureArguments

    $fixtureMetadata = Get-Content -LiteralPath (Join-Path $fixtureRoot "fixture.json") -Raw |
        ConvertFrom-Json
    $baselineId = [string]$fixtureMetadata.baseline_id
    $invalidId = [string]$fixtureMetadata.invalid_id
    $validId = [string]$fixtureMetadata.valid_id
    $activePointer = [string]$fixtureMetadata.active_pointer
    if ([string]::IsNullOrWhiteSpace($baselineId) -or
        [string]::IsNullOrWhiteSpace($invalidId) -or
        [string]::IsNullOrWhiteSpace($validId) -or
        -not (Test-Path -LiteralPath $activePointer -PathType Leaf)) {
        throw "Shader reload fixture metadata is incomplete."
    }
    if ($baselineId -eq $invalidId -or $baselineId -eq $validId -or $invalidId -eq $validId) {
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
        $text -match "rejected candidate=$escapedInvalid active=$escapedBaseline stage=integrity .*previous PSO retained"
    } "Invalid shader Generation was not rejected during integrity verification."
    if ($process.HasExited) {
        throw "ProjectHS exited after rejecting the invalid shader Generation."
    }

    $selectArguments = @($FixtureScript, "select", "--root", $fixtureRoot, "--candidate", "valid")
    Invoke-Fixture $selectArguments
    $escapedValid = [regex]::Escape($validId)
    Wait-For {
        $text = Read-ArtifactText $logPath
        $text -match "applied candidate=$escapedValid active=$escapedValid pipelines=1->2 parameters=1->1; temporal/HZB history invalidated"
    } "Valid shader Generation was not adopted."
    if ($process.HasExited) {
        throw "ProjectHS exited after adopting the valid shader Generation."
    }

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
