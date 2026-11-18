[CmdletBinding()]
param(
    [ValidateSet('verify-core', 'verify')]
    [string]$Workflow = 'verify-core',
    [Alias('Tests')]
    [string[]]$Test = @(),
    [string[]]$Label = @(),
    [switch]$All,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'msvc_environment.ps1')
$repoRoot = Split-Path $PSScriptRoot -Parent
if ($All) { $Workflow = 'verify' }
$preset = if ($Workflow -eq 'verify-core') { 'msvc-core' } else { 'msvc-debug' }
$configurePreset = if ($Workflow -eq 'verify-core') { 'msvc-core' } else { 'verify' }
$buildRoot = Join-Path $repoRoot "Build/$preset"
$exitCode = 1
$failure = $null
$selectedTests = @()
$phase = 'selection'
function Write-VerificationOutput {
    process {
        $line = [string]$_
        if ($Json) { [Console]::Error.WriteLine($line) }
        else { Write-Host $line }
    }
}

function Start-VerificationPhase([string]$Name) {
    $script:phase = $Name
    "[$Workflow] $Name" | Write-VerificationOutput
}

function Assert-NativeSuccess([string]$Message) {
    if ($LASTEXITCODE -ne 0) {
        $script:exitCode = $LASTEXITCODE
        throw $Message
    }
}

Push-Location $repoRoot
try {
    $selectorCount = [int]($PSBoundParameters.ContainsKey('Test')) + [int]($PSBoundParameters.ContainsKey('Label')) + [int][bool]$All
    if ($selectorCount -gt 1) { throw 'Select exactly one of -Test, -Label, or -All.' }
    if ($PSBoundParameters.ContainsKey('Test') -and !$Test) { throw '-Test requires exact CTest names.' }
    if ($PSBoundParameters.ContainsKey('Label') -and !$Label) { throw '-Label requires CTest label names.' }
    foreach ($entry in @($Test) + @($Label)) {
        if ([string]::IsNullOrWhiteSpace($entry) -or $entry -match '[\r\n]') {
            throw 'Test and label selectors require nonempty names without newlines.'
        }
    }
    Start-VerificationPhase 'environment'
    $installation = Initialize-HsMsvcEnvironment -BuildRoot $buildRoot
    $cmake = (& (Join-Path $PSScriptRoot 'bootstrap_cmake.ps1') -PrintPath 6>&1 |
        ForEach-Object {
            if ($_ -is [System.Management.Automation.InformationRecord]) { $_ | Write-VerificationOutput }
            else { $_ }
        } | Select-Object -Last 1)
    if (!$cmake -or !(Test-Path -LiteralPath $cmake -PathType Leaf)) { throw 'Repo-local CMake bootstrap did not return a valid executable.' }
    if (!(Get-Command ninja -ErrorAction SilentlyContinue)) {
        $bundledNinja = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
        if (Test-Path -LiteralPath $bundledNinja) {
            $env:PATH = (Split-Path $bundledNinja -Parent) + [IO.Path]::PathSeparator + $env:PATH
        }
    }
    $ctest = (Get-Command (Join-Path (Split-Path $cmake -Parent) 'ctest.exe') -ErrorAction Stop).Source
    Start-VerificationPhase 'configure'
    # CMake owns cache/preset reconciliation and buildsystem regeneration.
    $ErrorActionPreference = 'Continue'
    & $cmake --preset $configurePreset 2>&1 | Write-VerificationOutput
    Assert-NativeSuccess 'CMake configuration failed.'
    $ErrorActionPreference = 'Stop'
    Start-VerificationPhase 'selection'
    $selectionArguments = @()
    if ($Test) {
        $names = ($Test | ForEach-Object { [regex]::Escape($_) }) -join '|'
        $selectionArguments += @('--tests-regex', "^($names)$")
    }
    foreach ($labelName in $Label) {
        $selectionArguments += @('--label-regex', ('^' + [regex]::Escape($labelName) + '$'))
    }
    # Query the same selection CTest executes, including automatic fixture setup/cleanup.
    $ErrorActionPreference = 'Continue'
    $metadataJson = & $ctest --preset $preset @selectionArguments --show-only=json-v1 2>&1 |
        ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) { $_ | Write-VerificationOutput }
            else { $_ }
        } | Out-String
    Assert-NativeSuccess 'CTest selection failed.'
    $ErrorActionPreference = 'Stop'
    $metadata = $metadataJson | ConvertFrom-Json
    if ($metadata.kind -cne 'ctestInfo' -or $metadata.version.major -ne 1) {
        throw 'Unsupported CTest metadata schema.'
    }
    $selectedTests = @($metadata.tests | ForEach-Object { $_.name })
    foreach ($name in $Test) {
        if ($selectedTests -cnotcontains $name) { throw "Unknown exact CTest test name: $name" }
    }
    if (!$selectedTests.Count) { throw 'No configured tests were selected.' }
    $targets = @($selectedTests | ForEach-Object { "test_prep/$_" })
    $ErrorActionPreference = 'Continue'
    Start-VerificationPhase 'test-preparation'
    & $cmake --build $buildRoot --target @targets 2>&1 | Write-VerificationOutput
    Assert-NativeSuccess 'Selected CTest prerequisite preparation failed.'
    Start-VerificationPhase 'ctest'
    & $ctest --preset $preset @selectionArguments --no-tests=error --output-on-failure 2>&1 | Write-VerificationOutput
    Assert-NativeSuccess 'Selected CTest tests failed.'
    $exitCode = 0
} catch {
    $failure = $_.Exception.Message
    ($_ | Out-String) | Write-VerificationOutput
} finally {
    Pop-Location
    $result = [ordered]@{
        schema_version = 1; workflow = $Workflow; phase = $phase
        tests = $selectedTests
        exit_code = $exitCode; succeeded = ($exitCode -eq 0); error = $failure
    }
    if ($Json) { $result | ConvertTo-Json -Depth 4 }
    else {
        Write-Output "Verification exit=$exitCode"
    }
}
exit $exitCode
