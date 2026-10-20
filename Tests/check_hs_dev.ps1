[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Ninja)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$shell = (Get-Process -Id $PID).Path
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixtureRoot = Join-Path $temporaryRoot ('hs-dev-' + [guid]::NewGuid().ToString('N'))
$toolsRoot = Join-Path $fixtureRoot 'tools'
$buildRoot = Join-Path $fixtureRoot 'build'

function Assert([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}

function Invoke-Dev([string[]]$Arguments) {
    # Read native stderr directly; Windows PowerShell redirection formats ErrorRecords.
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $shell
    $start.Arguments = ((@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        (Join-Path $toolsRoot 'hs_dev.ps1')) + $Arguments) |
        ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }) -join ' '
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errorOutput = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        return [pscustomobject]@{
            code = $process.ExitCode; output = $output.Result; error = $errorOutput.Result
        }
    } finally { $process.Dispose() }
}

try {
    Assert (Test-Path -LiteralPath $Ninja -PathType Leaf) 'Ninja executable is required'
    New-Item -ItemType Directory -Path $toolsRoot, (Join-Path $buildRoot 'Actions/render/shader') | Out-Null
    Copy-Item (Join-Path $repoRoot 'tools/hs_dev.ps1'), (Join-Path $repoRoot 'tools/verification_metadata.ps1') -Destination $toolsRoot
    @"
CMAKE_GENERATOR:INTERNAL=Ninja
CMAKE_MAKE_PROGRAM:FILEPATH=$Ninja
"@ | Set-Content (Join-Path $buildRoot 'CMakeCache.txt')
    @'
rule CUSTOM_COMMAND
  command = unused
build native: phony
build Actions/render/shader/result.json: CUSTOM_COMMAND
'@ | Set-Content (Join-Path $buildRoot 'build.ninja')
    $actionPath = Join-Path $buildRoot 'Actions/render/shader/result.json'
    $action = @{ schema_version = 1; action_id = 'render.shader'; namespace_path = 'render/shader'; inputs = @(); products = @() }
    $action | ConvertTo-Json | Set-Content $actionPath
    @'
param([string[]]$CMakeArguments)
Write-Output 'build diagnostic'
exit 23
'@ | Set-Content (Join-Path $toolsRoot 'cmake.ps1')
    @'
param([string]$Workflow, [string[]]$Test, [string[]]$Label, [switch]$All, [switch]$Json)
@{ workflow = $Workflow; tests = @($Test); labels = @($Label); all = [bool]$All; json = [bool]$Json } | ConvertTo-Json
exit 19
'@ | Set-Content (Join-Path $toolsRoot 'verify.ps1')

    $buildArguments = @('build', '-BuildRoot', $buildRoot, '-Json')
    $result = Invoke-Dev ($buildArguments + @('-Target', 'native,render.shader,native', '-DryRun'))
    Assert ($result.code -eq 0 -and !$result.error) "dry-run failed: $($result.error)"
    $plan = $result.output | ConvertFrom-Json
    Assert ($plan.resolved_targets.Count -eq 2 -and $plan.resolved_targets[0] -ceq 'native' -and
        $plan.resolved_targets[1] -ceq 'Actions/render/shader/result.json') 'targets must resolve and deduplicate'
    Assert ($plan.resolutions[0].kind -ceq 'native' -and $plan.resolutions[1].kind -ceq 'action') 'native and Action targets must retain their authority'
    Assert ($plan.dry_run -and $plan.cmake_arguments[0] -ceq '--build' -and
        $plan.cmake_arguments[2] -ceq '--target') 'dry-run must describe the explicit build invocation'

    foreach ($target in @('all', 'native,,native', '../native', '--help', 'unknown')) {
        $result = Invoke-Dev ($buildArguments + @('-Target', $target, '-DryRun'))
        Assert ($result.code -ne 0 -and !$result.output) "unsafe or unknown target accepted: $target"
        Assert (($result.error | ConvertFrom-Json).code -ceq 'HS_DEV_FAILURE') 'failure must be JSON on stderr'
    }
    $action.namespace_path = '../escape'
    $action | ConvertTo-Json | Set-Content $actionPath
    $result = Invoke-Dev ($buildArguments + @('-Target', 'render.shader', '-DryRun'))
    Assert ($result.code -ne 0 -and ($result.error | ConvertFrom-Json).reason -match 'namespace mismatch') 'invalid Action namespace must fail'
    $result = Invoke-Dev ($buildArguments + @('-Target', 'native', '-DryRun'))
    Assert ($result.code -eq 0) 'native target must not require valid unrelated Action metadata'
    $result = Invoke-Dev ($buildArguments + @('-Target', 'native'))
    $plan = $result.output | ConvertFrom-Json
    Assert ($result.code -eq 23 -and $plan.exit_code -eq 23 -and !$plan.succeeded -and
        $result.error -match 'build diagnostic') 'build must preserve failure exit and keep diagnostics off JSON stdout'

    $result = Invoke-Dev @('verify', 'literal[.]test', '-Workflow', 'verify-core', '-Json')
    $forwarded = $result.output | ConvertFrom-Json
    Assert ($result.code -eq 19 -and $forwarded.workflow -ceq 'verify-core' -and
        $forwarded.tests[0] -ceq 'literal[.]test' -and $forwarded.json) 'verification must forward exact names, workflow, JSON and failure exit'
    $result = Invoke-Dev @('verify', '-Label', 'label[.]', '-Json')
    $forwarded = $result.output | ConvertFrom-Json
    Assert ($result.code -eq 19 -and $forwarded.labels[0] -ceq 'label[.]' -and $forwarded.workflow -ceq 'verify') 'label selection must remain delegated to verification'
    $result = Invoke-Dev @('verify', '-BuildRoot', $buildRoot, '-Json')
    Assert ($result.code -ne 0 -and !$result.output -and
        ($result.error | ConvertFrom-Json).code -ceq 'HS_DEV_FAILURE') 'verification must reject ignored build roots'
    Write-Output 'hs_dev explicit build and verification forwarding contracts passed.'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    if (!$resolved.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notmatch '^hs-dev-[0-9a-f]{32}$') {
        throw 'Refusing to remove an unexpected fixture path'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
