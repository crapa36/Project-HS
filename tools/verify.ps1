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
. (Join-Path $PSScriptRoot 'verification_metadata.ps1')
$repoRoot = Split-Path $PSScriptRoot -Parent
if ($All) { $Workflow = 'verify' }
$preset = if ($Workflow -eq 'verify-core') { 'msvc-core' } else { 'msvc-validation' }
$buildRoot = Join-Path $repoRoot "Build/$preset"
$logDirectory = Join-Path $repoRoot 'Build/verification'
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
$runId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + $PID
$logPath = Join-Path $logDirectory "$Workflow-$runId.log"
$testNamesPath = Join-Path $logDirectory "test-selection-$runId.txt"
$exitCode = 1
$failure = $null
$selectedTests = @()
$phase = 'selection'
$log = [IO.StreamWriter]::new($logPath, $true, [Text.UTF8Encoding]::new($false))
$log.AutoFlush = $true

function Write-VerificationOutput {
    process {
        $line = [string]$_
        $log.WriteLine($line)
        if ($Json) { [Console]::Error.WriteLine($line) }
        else { Write-Host $line }
    }
}

function Start-VerificationPhase([string]$Name) {
    $script:phase = $Name
    "[$Workflow] $Name; log=$logPath" | Write-VerificationOutput
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
    $cachePath = Join-Path $buildRoot 'CMakeCache.txt'
    $configuredCompiler = ''
    if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
        foreach ($cacheLine in Get-Content -LiteralPath $cachePath) {
            if ($cacheLine -match '^CMAKE_CXX_COMPILER:[^=]+=(.+)$') {
                $configuredCompiler = $matches[1].Replace('\', '/')
                break
            }
        }
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere.exe is required.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or !$installation) { throw 'MSVC x64 build tools were not found.' }
    $vcvarsVersionArgument = ''
    if ($configuredCompiler -match '^(?<installation>(?:[A-Za-z]:/|//)[^\r\n]+?)/VC/Tools/MSVC/(?<toolset>[0-9]+\.[0-9]+\.[0-9]+)/bin/Host[^/]+/[^/]+/cl\.exe$') {
        $installation = $matches.installation.Replace('/', '\')
        $configuredToolset = $matches.toolset
        $toolsetRoot = Join-Path $installation "VC/Tools/MSVC/$configuredToolset"
        $configuredDeveloperCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
        if (!(Test-Path -LiteralPath $configuredCompiler -PathType Leaf) -or
            !(Test-Path -LiteralPath $toolsetRoot -PathType Container) -or
            !(Test-Path -LiteralPath $configuredDeveloperCommand -PathType Leaf)) {
            throw "Configured MSVC toolset $configuredToolset is unavailable at $toolsetRoot."
        }
        $vcvarsVersionArgument = " -vcvars_ver=$configuredToolset"
    }
    $developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    # Import the developer environment once; subsequent commands need no nested shell quoting.
    $environmentLines = & $env:ComSpec /d /c "`"$developerCommand`" -no_logo -arch=x64 -host_arch=x64$vcvarsVersionArgument >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio developer environment initialization failed.' }
    $environmentNames = @{}
    foreach ($line in $environmentLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            $environmentName = $matches[1]
            if (!$environmentNames.ContainsKey($environmentName)) {
                $environmentNames[$environmentName] = $true
                [Environment]::SetEnvironmentVariable($environmentName, $matches[2], 'Process')
            }
        }
    }
    $env:VSLANG = '1033'
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
    & $cmake --preset $preset -DHS_BUILD_TESTS=ON 2>&1 | Write-VerificationOutput
    Assert-NativeSuccess 'CMake configuration failed.'
    $ErrorActionPreference = 'Stop'
    Start-VerificationPhase 'selection'
    $selectionArguments = @()
    if ($Test) {
        [IO.File]::WriteAllLines($testNamesPath, $Test, [Text.UTF8Encoding]::new($false))
        $selectionArguments += @('--tests-from-file', $testNamesPath)
    }
    foreach ($labelName in $Label) {
        $selectionArguments += @('--label-regex', ('^' + [regex]::Escape($labelName) + '$'))
    }
    # Query the same selection CTest executes, including automatic fixture setup/cleanup.
    $metadataArguments = @{ CTest = $ctest; BuildRoot = $buildRoot; Preset = $preset; Labels = $Label }
    if ($Test) { $metadataArguments.TestNamesFile = $testNamesPath }
    $metadata = Get-HsCTestMetadata @metadataArguments
    # CTest ignores unknown names in a selection file, even beside valid ones.
    if ($Test) { Select-HsExactTests $metadata $Test | Out-Null }
    $selectedTests = @($metadata.tests)
    if (!$selectedTests.Count) { throw 'No configured tests were selected.' }
    Start-VerificationPhase 'build'
    $ErrorActionPreference = 'Continue'
    if (!$Test -and !$Label) {
        & $cmake --build --preset $preset 2>&1 | Write-VerificationOutput
        Assert-NativeSuccess 'Full workflow build failed.'
    }
    $targets = @($selectedTests | ForEach-Object { "test_prep/$($_.name)" })
    Start-VerificationPhase 'test-preparation'
    & $cmake --build --preset $preset --target @targets 2>&1 | Write-VerificationOutput
    Assert-NativeSuccess 'Selected CTest prerequisite preparation failed.'
    Start-VerificationPhase 'ctest'
    & $ctest --preset $preset @selectionArguments --no-tests=error --output-on-failure 2>&1 | Write-VerificationOutput
    Assert-NativeSuccess 'Selected CTest tests failed.'
    $exitCode = 0
} catch {
    $failure = $_.Exception.Message
    ($_ | Out-String) | Write-VerificationOutput
} finally {
    $log.Dispose()
    Pop-Location
    $result = [ordered]@{
        schema_version = 1; workflow = $Workflow; phase = $phase
        tests = @($selectedTests | ForEach-Object { [string]$_.name })
        exit_code = $exitCode; succeeded = ($exitCode -eq 0); error = $failure; log = $logPath
    }
    if ($Json) { $result | ConvertTo-Json -Depth 4 }
    else {
        Write-Output "Verification exit=$exitCode; log=$logPath"
    }
}
exit $exitCode
