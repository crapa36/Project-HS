param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$Shader,
    [Parameter(Mandatory = $true)][string]$Artifacts
)

$ErrorActionPreference = "Stop"
$original = [System.IO.File]::ReadAllBytes($Shader)
$process = $null
$restored = $false

function Wait-For([scriptblock]$Condition, [string]$Failure) {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $Condition) { return }
        if ($process -and $process.HasExited) {
            throw "$Failure Process exited with code $($process.ExitCode)."
        }
        Start-Sleep -Milliseconds 100
    }
    throw $Failure
}

try {
    New-Item -ItemType Directory -Force -Path $Artifacts | Out-Null
    $log = Join-Path $Artifacts "shader_hot_reload.log"
    Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $Artifacts "adapter.txt") `
        -Force -ErrorAction SilentlyContinue
    $arguments = "--artifacts=`"$Artifacts`" --no-vsync --frame-cap=60"
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments `
        -WindowStyle Hidden -PassThru

    Wait-For { Test-Path (Join-Path $Artifacts "adapter.txt") } `
        "Renderer did not initialize."
    Start-Sleep -Seconds 1

    [System.IO.File]::WriteAllBytes($Shader, [byte[]](0, 1, 2, 3))
    [System.IO.File]::SetLastWriteTimeUtc($Shader, [DateTime]::UtcNow.AddSeconds(2))
    Wait-For { (Test-Path $log) -and ((Get-Content $log -Raw) -match "rejected .*previous PSO retained") } `
        "Invalid shader was not rejected."

    [System.IO.File]::WriteAllBytes($Shader, $original)
    [System.IO.File]::SetLastWriteTimeUtc($Shader, [DateTime]::UtcNow.AddSeconds(4))
    $restored = $true
    Wait-For { (Get-Content $log -Raw) -match "applied" } `
        "Valid shader was not restored after rejection."
}
finally {
    if (-not $restored) {
        [System.IO.File]::WriteAllBytes($Shader, $original)
    }
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
    }
}
