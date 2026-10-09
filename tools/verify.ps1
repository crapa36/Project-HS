[CmdletBinding()]
param(
    [ValidateSet('verify-core', 'verify')]
    [string]$Workflow = 'verify-core',
    [Alias('Tests')]
    [string[]]$Test = @(),
    [string[]]$Label = @(),
    [switch]$Changed,
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
$resultPath = Join-Path $logDirectory "$Workflow-$runId.json"
$started = [DateTime]::UtcNow
$exitCode = 1
$phase = 'selection'
$failure = $null
$selectedTests = @()
$preparations = @()
$scopes = @()
$analysis = $null
$selectionMode = 'workflow'
$durations = [ordered]@{}
$phaseStarted = $started
$testResults = @()
$fullGate = $false
$workflowGate = $false
$configured = $false
$testNamesPath = Join-Path $logDirectory "test-selection-$runId.txt"
$junitPath = Join-Path $logDirectory "test-results-$runId.xml"

function Write-VerificationDiagnostic([string]$Message) {
    if ($Json) { [Console]::Error.WriteLine($Message) } else { Write-Host $Message }
}

function Start-VerificationPhase([string]$Name) {
    $now = [DateTime]::UtcNow
    $script:durations[$script:phase] = [Math]::Round([double]$script:durations[$script:phase] + ($now - $script:phaseStarted).TotalMilliseconds, 3)
    $script:phase = $Name
    $script:phaseStarted = $now
}

function Assert-NativeSuccess([string]$Message) {
    if ($LASTEXITCODE -ne 0) {
        $script:exitCode = $LASTEXITCODE
        throw $Message
    }
}

function Add-PresetCacheValues {
    param([string]$Name, [object]$Presets, [hashtable]$Values)

    $preset = $Presets.configurePresets | Where-Object { $_.name -eq $Name } | Select-Object -First 1
    if (!$preset) { throw "Unknown configure preset: $Name" }
    if ($preset.inherits) {
        foreach ($baseName in @($preset.inherits)) {
            Add-PresetCacheValues -Name $baseName -Presets $Presets -Values $Values
        }
    }
    if ($preset.cacheVariables) {
        foreach ($entry in $preset.cacheVariables.PSObject.Properties) {
            if ($null -eq $entry.Value) {
                $Values.Remove($entry.Name) | Out-Null
            }
            elseif ($entry.Value -is [string]) {
                $Values[$entry.Name] = $entry.Value
            }
            else {
                $Values[$entry.Name] = [string]$entry.Value.value
            }
        }
    }
}

Push-Location $repoRoot
try {
    $selectorCount = [int]($PSBoundParameters.ContainsKey('Test')) + [int]($PSBoundParameters.ContainsKey('Label')) + [int][bool]$Changed + [int][bool]$All
    if ($selectorCount -gt 1) { throw 'Select exactly one of -Test, -Label, -Changed, or -All.' }
    if ($PSBoundParameters.ContainsKey('Test') -and @($Test).Count -eq 0) { throw '-Test requires exact CTest names.' }
    if ($PSBoundParameters.ContainsKey('Label') -and @($Label).Count -eq 0) { throw '-Label requires CTest label names.' }
    foreach ($entry in @($Test) + @($Label)) {
        if ([string]::IsNullOrWhiteSpace($entry) -or $entry -match '[\r\n]') {
            throw 'Test and label selectors require nonempty names without newlines.'
        }
    }
    if ($PSBoundParameters.ContainsKey('Test')) { $selectionMode = 'tests' }
    elseif ($PSBoundParameters.ContainsKey('Label')) { $selectionMode = 'labels' }
    elseif ($Changed) { $selectionMode = 'changed' }
    elseif ($All) { $selectionMode = 'all' }
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
    $cmake = (& (Join-Path $PSScriptRoot 'bootstrap_cmake.ps1') -PrintPath 6>> $logPath | Select-Object -Last 1)
    if (!$cmake -or !(Test-Path -LiteralPath $cmake -PathType Leaf)) { throw 'Repo-local CMake bootstrap did not return a valid executable.' }
    if (!(Get-Command ninja -ErrorAction SilentlyContinue)) {
        $bundledNinja = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
        if (Test-Path -LiteralPath $bundledNinja) {
            $env:PATH = (Split-Path $bundledNinja -Parent) + [IO.Path]::PathSeparator + $env:PATH
        }
    }
    $ctest = (Get-Command (Join-Path (Split-Path $cmake -Parent) 'ctest.exe') -ErrorAction Stop).Source
    $ninja = (Get-Command ninja -ErrorAction Stop).Source
    Start-VerificationPhase 'configure'
    $needsConfigure = !(Test-Path -LiteralPath $cachePath -PathType Leaf) -or
        !(Test-Path -LiteralPath (Join-Path $buildRoot 'build.ninja') -PathType Leaf)
    $cacheValues = @{}
    if (!$needsConfigure) {
        foreach ($cacheLine in Get-Content -LiteralPath $cachePath) {
            if ($cacheLine -match '^([^#/:][^:]*):[^=]+=(.*)$') { $cacheValues[$matches[1]] = $matches[2] }
        }
        $needsConfigure = !$cacheValues.ContainsKey('CMAKE_COMMAND') -or
            ![StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFullPath([string]$cacheValues.CMAKE_COMMAND), [IO.Path]::GetFullPath($cmake)) -or
            !$cacheValues.ContainsKey('CMAKE_HOME_DIRECTORY') -or
            ![StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFullPath([string]$cacheValues.CMAKE_HOME_DIRECTORY), [IO.Path]::GetFullPath($repoRoot))
        if (!$needsConfigure) {
            $cacheUpdated = (Get-Item -LiteralPath $cachePath).LastWriteTimeUtc
            foreach ($presetFile in @('CMakePresets.json', 'CMakeUserPresets.json')) {
                $presetPath = Join-Path $repoRoot $presetFile
                if ((Test-Path -LiteralPath $presetPath -PathType Leaf) -and
                    (Get-Item -LiteralPath $presetPath).LastWriteTimeUtc -gt $cacheUpdated) { $needsConfigure = $true }
            }
        }
        if (!$needsConfigure) {
            $presetDocument = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakePresets.json') -Raw | ConvertFrom-Json
            $expectedValues = @{}
            Add-PresetCacheValues -Name $preset -Presets $presetDocument -Values $expectedValues
            foreach ($name in $expectedValues.Keys) {
                if (!$cacheValues.ContainsKey($name) -or
                    ![StringComparer]::OrdinalIgnoreCase.Equals([string]$cacheValues[$name], [string]$expectedValues[$name])) {
                    $needsConfigure = $true
                    break
                }
            }
        }
    }
    # Native stderr in Windows PowerShell is diagnostic data; inspect exit codes.
    $ErrorActionPreference = 'Continue'
    if ($needsConfigure) {
        & $cmake --preset $preset *>> $logPath
        Assert-NativeSuccess 'CMake configuration failed.'
        $configured = $true
    }
    Start-VerificationPhase 'regeneration'
    # This target regenerates stale CMake inputs without building test prerequisites.
    & $ninja -C $buildRoot build.ninja *>> $logPath
    Assert-NativeSuccess 'CMake/Ninja buildsystem regeneration failed.'
    $ErrorActionPreference = 'Stop'
    Start-VerificationPhase 'metadata'
    $metadata = Get-HsCTestMetadata -CTest $ctest -BuildRoot $buildRoot -Preset $preset
    Start-VerificationPhase 'selection'
    if ($Test) { $selectedTests = @(Select-HsExactTests $metadata $Test) }
    elseif ($Label) {
        $selectedTests = @((Get-HsCTestMetadata -CTest $ctest -BuildRoot $buildRoot -Preset $preset -Labels $Label).tests)
        if ($selectedTests.Count -eq 0) { throw 'CTest label selection matched no configured tests.' }
    } elseif ($Changed) {
        $analysis = Get-HsAffectedTests $repoRoot $buildRoot $metadata $ninja
        $selectedTests = @(Select-HsExactTests $metadata $analysis.tests)
    } else { $selectedTests = @($metadata.tests) }
    if (!$Changed -and $selectedTests.Count -eq 0) { throw 'No configured tests were selected.' }
    Start-VerificationPhase 'metadata'
    $queryCache = @{}
    $preparations = @(
        foreach ($testMetadata in $selectedTests) {
            Get-HsTestPreparation $ninja $buildRoot ([string]$testMetadata.name) $queryCache
        }
    )
    $scopes = @(Get-HsGenerationScopes $selectedTests $preparations)
    Start-VerificationPhase 'generation-preflight'
    if ($Workflow -eq 'verify') {
        $generationExecutable = Join-Path $buildRoot 'hs_generation.exe'
        $queuedRepairs = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($scope in $scopes) {
            $generationPointer = Join-Path $buildRoot "GenerationState/$scope.json"
            if (!(Test-Path -LiteralPath $generationExecutable -PathType Leaf) -or
                !(Test-Path -LiteralPath $generationPointer -PathType Leaf)) { continue }
            $existingPointer = Get-Content -LiteralPath $generationPointer -Raw | ConvertFrom-Json
            if ($existingPointer.schema_version -eq 1 -and
                [string]$existingPointer.scope -ceq $scope -and
                [string]$existingPointer.generation_id -cmatch '^[0-9a-f]{64}$' -and
                [string]$existingPointer.manifest_locator -ceq
                    "Generations/$($existingPointer.generation_id)/generation.json") {
                $existingManifest = Get-Content -LiteralPath (Join-Path $buildRoot $existingPointer.manifest_locator) -Raw | ConvertFrom-Json
                if ($existingManifest.schema_version -eq 1 -and
                    [string]$existingManifest.generation_id -ceq
                        [string]$existingPointer.generation_id -and
                    [string]$existingManifest.scope -ceq $scope) {
                    # The newly built composer will replace this legacy pointer.
                    continue
                }
            }
            $ErrorActionPreference = 'Continue'
            $generationCheck = (& $generationExecutable --verify --build-root $buildRoot --scope $scope 2>&1 |
                ForEach-Object { $_.ToString() } | Out-String).Trim()
            $generationExitCode = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            if ($generationExitCode -eq 0) { continue }
            try {
                if (!$generationCheck.StartsWith('{') -or !$generationCheck.EndsWith('}')) {
                    throw 'Expected one diagnostic object.'
                }
                $diagnostic = $generationCheck | ConvertFrom-Json -ErrorAction Stop
                $fields = @($diagnostic.PSObject.Properties.Name)
                $textFields = @('producer_id', 'error_code', 'action_id', 'reason', 'build_root', 'scope')
                if ($diagnostic -isnot [pscustomobject] -or
                    @(@('schema_version', 'verify') + $textFields | Where-Object { $fields -cnotcontains $_ }).Count -ne 0 -or
                    @($textFields | Where-Object { $diagnostic.PSObject.Properties[$_].Value -isnot [string] }).Count -ne 0 -or
                    !($diagnostic.schema_version -is [int] -or $diagnostic.schema_version -is [long]) -or
                    $diagnostic.schema_version -ne 1 -or
                    $diagnostic.producer_id -cne 'hs_generation' -or
                    $diagnostic.error_code -cnotin @('missing-product', 'corrupt-product') -or
                    $diagnostic.action_id -cne '-' -or
                    [string]::IsNullOrWhiteSpace($diagnostic.reason) -or
                    $diagnostic.scope -cne $scope -or
                    ![StringComparer]::OrdinalIgnoreCase.Equals($diagnostic.build_root, $buildRoot) -or
                    $diagnostic.verify -isnot [bool] -or !$diagnostic.verify) {
                    throw 'Diagnostic does not identify a repairable product in the requested Generation.'
                }
            } catch {
                throw "Generation preflight failed: $generationCheck"
            }
            $pointer = Get-Content -LiteralPath $generationPointer -Raw | ConvertFrom-Json
            $manifestPath = Join-Path $buildRoot $pointer.manifest_locator
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
            $corruptActions = [System.Collections.Generic.HashSet[string]]::new(
                [System.StringComparer]::Ordinal)
            foreach ($product in $manifest.products) {
                $filename = [IO.Path]::GetFileName($product.locator)
                if ($manifest.schema_version -eq 3) {
                    $hasher = [Security.Cryptography.SHA256]::Create()
                    try {
                        $nameHash = ([BitConverter]::ToString($hasher.ComputeHash(
                            [Text.Encoding]::UTF8.GetBytes([string]$product.logical_id)))).Replace('-', '').ToLowerInvariant()
                    } finally { $hasher.Dispose() }
                    $expectedLocator = "Generations/$($pointer.generation_id)/products/$nameHash.bin"
                } elseif ($manifest.schema_version -eq 2 -and
                    $product.product_set_id -cmatch '^[0-9a-f]{64}$') {
                    $expectedLocator = "Products/$($product.namespace_path)/$($product.product_set_id)/$filename"
                } else {
                    throw "Generation repair found an unsupported schema or invalid ProductSet."
                }
                if ($product.namespace_path -cnotmatch '^[A-Za-z0-9._-]{1,128}(/[A-Za-z0-9._-]{1,128})*$' -or
                    @(([string]$product.namespace_path) -split '/' | Where-Object { $_ -eq '.' -or $_ -eq '..' }).Count -gt 0 -or
                    $filename -cnotmatch '^[a-zA-Z0-9._-]+$' -or
                    $filename -in @('.', '..') -or
                    $product.locator -cne $expectedLocator -or
                    $product.digest -cnotmatch '^sha256:[0-9a-f]{64}$') {
                    throw "Generation repair found an unsafe product locator or digest."
                }
                $productPath = Join-Path $buildRoot $product.locator
                $corrupt = !(Test-Path -LiteralPath $productPath -PathType Leaf)
                if (!$corrupt) {
                    $file = Get-Item -LiteralPath $productPath -Force
                    $corrupt = $file.Length -ne [long]$product.size -or
                        (Get-FileHash -LiteralPath $productPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne
                            $product.digest.Substring(7)
                }
                if ($corrupt) { [void]$corruptActions.Add($product.action_id) }
            }
            if ($corruptActions.Count -eq 0) {
                throw "Generation preflight failed without a repairable product: $generationCheck"
            }
            foreach ($actionId in $corruptActions) {
                if ($queuedRepairs.Contains($actionId)) { continue }
                $product = $manifest.products | Where-Object { $_.action_id -eq $actionId } | Select-Object -First 1
                if (!$product -or $actionId -notmatch '^[a-zA-Z0-9._-]+$') {
                    throw "Generation repair cannot identify a safe ActionResult for $actionId."
                }
                $actionResult = Join-Path $buildRoot "Actions/$($product.namespace_path)/result.json"
                if (!(Test-Path -LiteralPath $actionResult -PathType Leaf)) {
                    throw "Generation repair cannot find ActionResult for $actionId."
                }
                $cursor = [IO.Path]::GetFullPath($actionResult)
                $repoPrefix = [IO.Path]::GetFullPath($repoRoot) + [IO.Path]::DirectorySeparatorChar
                while ($cursor.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                    $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
                    if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                        throw "Refusing Generation repair through a reparse point: $cursor"
                    }
                    $cursor = Split-Path $cursor -Parent
                }
                Remove-Item -LiteralPath $actionResult -Force
                [void]$queuedRepairs.Add($actionId)
                Write-VerificationDiagnostic "Generation $scope product repair queued through CMake/Ninja: $actionId"
            }
        }
    }
    Write-VerificationDiagnostic "Running $Workflow ($selectionMode, $($selectedTests.Count) tests). Full log: $logPath"
    $workflowGate = $selectionMode -in @('workflow', 'all')
    $fullGate = $Workflow -eq 'verify' -and $workflowGate
    $ErrorActionPreference = 'Continue'
    Start-VerificationPhase 'preparation'
    if ($workflowGate) {
        # Preserve the workflow's complete build gate. Targeted runs use only prep.
        & $cmake --build --preset $preset *>> $logPath
        Assert-NativeSuccess 'Full workflow build failed.'
    }
    if ($selectedTests.Count -gt 0) {
        $targets = @($preparations | ForEach-Object { [string]$_.target })
        & $cmake --build --preset $preset --target @targets *>> $logPath
        Assert-NativeSuccess 'Selected CTest prerequisite preparation failed.'
        [IO.File]::WriteAllLines($testNamesPath, [string[]]@($selectedTests | ForEach-Object { [string]$_.name }), [Text.UTF8Encoding]::new($false))
        Start-VerificationPhase 'ctest'
        $ctestArguments = @('--preset', $preset, '--tests-from-file', $testNamesPath, '--no-tests=error', '--output-junit', $junitPath)
        foreach ($labelName in @($Label)) {
            $ctestArguments += @('--label-regex', ('^' + [regex]::Escape($labelName) + '$'))
        }
        & $ctest @ctestArguments *>> $logPath
        Assert-NativeSuccess 'Selected CTest tests failed.'
    } else {
        Write-VerificationDiagnostic 'Changed selection contains no tests; no prerequisites or tests were run.'
    }
    $exitCode = 0
    $ErrorActionPreference = 'Stop'
    if (Test-Path -LiteralPath $logPath -PathType Leaf) {
        Get-Content -LiteralPath $logPath -Tail 8 | ForEach-Object { Write-VerificationDiagnostic ([string]$_) }
    }
}
catch {
    $_ | Out-String | Add-Content -LiteralPath $logPath
    Write-VerificationDiagnostic ([string]$_)
    if ($exitCode -eq 0) { $exitCode = 1 }
    $codes = @{
        selection = 'HS_VERIFY_SELECTION_INVALID'
        environment = 'HS_VERIFY_ENVIRONMENT_UNAVAILABLE'
        configure = 'HS_VERIFY_CONFIGURE_FAILED'
        regeneration = 'HS_VERIFY_REGENERATION_FAILED'
        metadata = 'HS_VERIFY_METADATA_FAILED'
        'generation-preflight' = 'HS_VERIFY_GENERATION_PREFLIGHT_FAILED'
        preparation = 'HS_VERIFY_PREPARATION_FAILED'
        ctest = 'HS_VERIFY_TEST_FAILED'
    }
    $remediation = @{
        selection = 'Use one selection mode with nonempty exact configured test names or labels.'
        environment = 'Install the MSVC x64 build tools and restore the repo-local CMake/Ninja tools.'
        configure = 'Correct the CMake configuration error recorded in the log.'
        regeneration = 'Correct the changed CMake input or unavailable prerequisite recorded in the log.'
        metadata = 'Use configured CTest names/labels and ensure test_prep targets exist.'
        'generation-preflight' = 'Inspect the selected Generation and ActionResult dependency reported in the log.'
        preparation = 'Correct the selected prerequisite build error recorded in the log.'
        ctest = 'Inspect failed test results and fix the test or its selected dependencies.'
    }
    $failure = [ordered]@{
        code = $codes[$phase]
        phase = $phase
        reason = $_.Exception.Message
        tests = @($selectedTests | ForEach-Object { [string]$_.name })
        generation_scopes = @($scopes)
        relevant_dependencies = @($preparations | ForEach-Object { $_.dependencies } | Sort-Object -Unique)
        remediation = $remediation[$phase]
    }
    if (Test-Path -LiteralPath $logPath -PathType Leaf) {
        Get-Content -LiteralPath $logPath -Tail 60 | ForEach-Object { Write-VerificationDiagnostic ([string]$_) }
    }
}
finally {
    $finished = [DateTime]::UtcNow
    $durations[$phase] = [Math]::Round([double]$durations[$phase] + ($finished - $phaseStarted).TotalMilliseconds, 3)
    if (Test-Path -LiteralPath $junitPath -PathType Leaf) {
        try {
            [xml]$junit = Get-Content -LiteralPath $junitPath -Raw
            $testResults = @(
                foreach ($testCase in $junit.SelectNodes('//testcase')) {
                    [ordered]@{
                        name = [string]$testCase.name
                        elapsed_seconds = [double]::Parse([string]$testCase.time, [Globalization.CultureInfo]::InvariantCulture)
                        status = $(if ($testCase.SelectSingleNode('failure')) { 'failed' } elseif ($testCase.SelectSingleNode('skipped')) { 'skipped' } else { 'passed' })
                    }
                }
            )
        } catch { Write-VerificationDiagnostic "CTest JUnit receipt could not be read: $($_.Exception.Message)" }
    }
    $receipt = [ordered]@{
        schema_version = 1
        workflow = $Workflow
        preset = $preset
        build_root = $buildRoot
        selection_mode = $selectionMode
        test_name = $(if ($Test.Count -eq 1) { $Test[0] } else { $null })
        requested_tests = @($Test)
        requested_labels = @($Label)
        selected_tests = @(Get-HsTestSummary $selectedTests $preparations)
        generation_scopes = @($scopes)
        affected = $analysis
        configured = $configured
        workflow_gate = $workflowGate
        full_gate = $fullGate
        full_gate_required_for_merge = $true
        started_utc = $started.ToString('o')
        finished_utc = $finished.ToString('o')
        elapsed_ms = [Math]::Round(($finished - $started).TotalMilliseconds, 3)
        phase_durations_ms = $durations
        test_results = $testResults
        exit_code = $exitCode
        failure = $failure
        log = $logPath
        test_selection_file = $(if (Test-Path -LiteralPath $testNamesPath -PathType Leaf) { $testNamesPath } else { $null })
        test_results_file = $(if (Test-Path -LiteralPath $junitPath -PathType Leaf) { $junitPath } else { $null })
        receipt = $resultPath
        note = 'Execution receipt only; not permission to reuse results after source, input, toolchain, or environment changes.'
    }
    $receiptJson = $receipt | ConvertTo-Json -Depth 20
    $receiptJson | Set-Content -LiteralPath $resultPath -Encoding UTF8
    Pop-Location
    if ($Json) { Write-Output $receiptJson }
    else { Write-VerificationDiagnostic "Receipt: $resultPath" }
}
exit $exitCode
