[CmdletBinding()]
param(
    [ValidateSet('verify-core', 'verify')]
    [string]$Workflow = 'verify-core',
    [string]$Target,
    [string]$Test
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$logDirectory = Join-Path $repoRoot 'Build/verification'
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
$runId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + $PID
$logPath = Join-Path $logDirectory "$Workflow-$runId.log"
$resultPath = Join-Path $logDirectory "$Workflow-$runId.json"
$started = [DateTime]::UtcNow
$exitCode = 1

Push-Location $repoRoot
try {
    if ([bool]$Target -ne [bool]$Test) { throw 'Target and Test must be supplied together for a targeted build/test.' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere.exe is required.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or !$installation) { throw 'MSVC x64 build tools were not found.' }
    $developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    # Import the developer environment once; subsequent commands need no nested shell quoting.
    $environmentLines = & $env:ComSpec /d /c "`"$developerCommand`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio developer environment initialization failed.' }
    foreach ($line in $environmentLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    $env:VSLANG = '1033'
    $cmake = (Get-Command cmake -ErrorAction Stop).Source
    if ($Target) {
        $ctest = (Get-Command (Join-Path (Split-Path $cmake -Parent) 'ctest.exe') -ErrorAction Stop).Source
    }
    Get-Command ninja -ErrorAction Stop | Out-Null
    Write-Host "Running $Workflow. Full log: $logPath"
    # Windows PowerShell wraps native stderr as ErrorRecord; use native exit codes.
    $ErrorActionPreference = 'Continue'
    if ($Target) {
        $preset = if ($Workflow -eq 'verify-core') { 'msvc-core' } else { 'msvc-validation' }
        & $cmake --build --preset $preset --target $Target *> $logPath
        $exitCode = $LASTEXITCODE
        if ($exitCode -eq 0) {
            & $ctest --preset $preset -R $Test --no-tests=error *>> $logPath
            $exitCode = $LASTEXITCODE
        }
    }
    else {
        & $cmake --workflow --preset $Workflow *> $logPath
        $exitCode = $LASTEXITCODE
    }
    $ErrorActionPreference = 'Stop'
    if ($exitCode -ne 0) { Get-Content -LiteralPath $logPath -Tail 60 | Write-Host }
    else { Get-Content -LiteralPath $logPath -Tail 8 | Write-Host }
}
catch {
    $_ | Out-String | Add-Content -LiteralPath $logPath
    Write-Host $_
    $exitCode = 1
}
finally {
    [ordered]@{
        workflow = $Workflow
        target = $Target
        test_filter = $Test
        started_utc = $started.ToString('o')
        finished_utc = [DateTime]::UtcNow.ToString('o')
        exit_code = $exitCode
        log = $logPath
        note = 'Execution receipt only; not permission to reuse results after source, input, toolchain, or environment changes.'
    } | ConvertTo-Json | Set-Content -LiteralPath $resultPath -Encoding UTF8
    Pop-Location
}
exit $exitCode
