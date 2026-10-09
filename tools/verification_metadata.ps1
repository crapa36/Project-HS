# Shared, read-only views of the configured CTest/Ninja authorities.
# These queries support conservative development planning, never a release gate.
function Get-HsCTestProperty($TestMetadata, [string]$Name) {
    $property = @($TestMetadata.properties | Where-Object { $_.name -ceq $Name })
    if ($property.Count -gt 0) { return $property[0].value }
    return
}

function Get-HsCTestMetadata([string]$CTest, [string]$BuildRoot, [string[]]$Labels = @(), [string]$Preset) {
    $ErrorActionPreference = 'Continue'
    $arguments = if ($Preset) { @('--preset', $Preset) } else { @('--test-dir', $BuildRoot) }
    foreach ($label in $Labels) {
        if ([string]::IsNullOrWhiteSpace($label) -or $label -match '[\r\n]') {
            throw 'Labels must be nonempty CTest label names.'
        }
        # CTest's label selector is a regex; quote the requested label literally.
        $arguments += @('--label-regex', ('^' + [regex]::Escape($label) + '$'))
    }
    $diagnosticPath = [IO.Path]::GetTempFileName()
    try {
        $json = & $CTest @arguments --show-only=json-v1 2> $diagnosticPath | Out-String
        if ($LASTEXITCODE -ne 0) {
            throw "CTest metadata query failed: $(Get-Content -LiteralPath $diagnosticPath -Raw)"
        }
        $metadata = $json | ConvertFrom-Json
        if ($metadata.kind -cne 'ctestInfo' -or $metadata.version.major -ne 1) {
            throw 'Unsupported CTest metadata schema.'
        }
        $names = @{}
        foreach ($testMetadata in @($metadata.tests)) {
            $name = [string]$testMetadata.name
            if (!$name -or $name -match '[\r\n]' -or $names.ContainsKey($name)) {
                throw "Invalid or duplicate CTest test name: $name"
            }
            $names[$name] = $true
        }
        return $metadata
    } finally { Remove-Item -LiteralPath $diagnosticPath -Force -ErrorAction SilentlyContinue }
}

function Select-HsExactTests($Metadata, [string[]]$Names) {
    $selected = [System.Collections.Generic.List[object]]::new()
    $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($name in $Names) {
        if ([string]::IsNullOrWhiteSpace($name) -or $name -match '[\r\n]') {
            throw 'Test selection requires nonempty exact CTest names.'
        }
        $testMetadata = @($Metadata.tests | Where-Object { $_.name -ceq $name })
        if ($testMetadata.Count -ne 1) { throw "Unknown exact CTest test name: $name" }
        if ($seen.Add($name)) { $selected.Add($testMetadata[0]) }
    }
    return $selected.ToArray()
}

function Get-HsNinjaQuery([string]$Ninja, [string]$BuildRoot, [string]$Target, [hashtable]$Cache = @{}) {
    $ErrorActionPreference = 'Continue'
    if ($Cache.ContainsKey($Target)) { return $Cache[$Target] }
    $query = @(& $Ninja -C $BuildRoot -t query $Target 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Ninja dependency query failed for $Target`: $([string]::Join(' ', $query))"
    }
    $dependencies = @()
    $inInputs = $false
    $rule = ''
    foreach ($lineObject in $query) {
        $line = [string]$lineObject
        if ($line -match '^  input: (.+)$') { $inInputs = $true; $rule = $matches[1]; continue }
        if ($line -match '^  outputs:') { $inInputs = $false; continue }
        if ($inInputs -and $line.StartsWith('    ')) {
            $dependencies += $line.Substring(4).TrimStart('|', ' ')
        }
    }
    $result = [pscustomobject]@{ rule = $rule; dependencies = @($dependencies | Sort-Object -Unique) }
    $Cache[$Target] = $result
    return $result
}

function Get-HsTestPreparation([string]$Ninja, [string]$BuildRoot, [string]$Name, [hashtable]$Cache = @{}) {
    $target = "test_prep/$Name"
    $query = Get-HsNinjaQuery $Ninja $BuildRoot $target $Cache
    $scopes = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $visited = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $queue = [System.Collections.Generic.Queue[string]]::new()
    $queue.Enqueue($target)
    while ($queue.Count -gt 0) {
        $current = $queue.Dequeue()
        if (!$visited.Add($current)) { continue }
        $currentQuery = Get-HsNinjaQuery $Ninja $BuildRoot $current $Cache
        # Generation composition is reachable through phony prerequisite targets.
        # Stop at executable/custom command rules instead of inspecting recipes.
        if ($currentQuery.rule -cne 'phony') { continue }
        foreach ($dependency in $currentQuery.dependencies) {
            if ($dependency -cmatch '(^|[/\\])GenerationState[/\\]([A-Za-z0-9._-]+)\.json$') {
                [void]$scopes.Add($matches[2])
            } elseif ($dependency -notmatch '[/\\]|\.') {
                $queue.Enqueue($dependency)
            }
        }
    }
    return [pscustomobject][ordered]@{
        target = $target
        dependencies = @($query.dependencies)
        generation_scopes = @($scopes | Sort-Object)
        authority = 'configured-ninja-test-preparation'
    }
}

function Get-HsGenerationScopes($Tests, $Preparations = @()) {
    $scopes = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($testMetadata in $Tests) {
        foreach ($value in @(Get-HsCTestProperty $testMetadata 'ENVIRONMENT')) {
            if ([string]$value -cmatch '^HS_TEST_GENERATION_SCOPE=([A-Za-z0-9._-]+)$') {
                [void]$scopes.Add($matches[1])
            }
            if ([string]$value -cmatch '^HS_TEST_GENERATION_POINTER=.*[/\\]GenerationState[/\\]([A-Za-z0-9._-]+)\.json$') {
                [void]$scopes.Add($matches[1])
            }
        }
        foreach ($argument in @($testMetadata.command)) {
            if ([string]$argument -cmatch '[/\\]GenerationState[/\\]([A-Za-z0-9._-]+)\.json$') {
                [void]$scopes.Add($matches[1])
            }
        }
    }
    foreach ($preparation in @($Preparations)) {
        foreach ($scope in @($preparation.generation_scopes)) { [void]$scopes.Add([string]$scope) }
    }
    return @($scopes | Sort-Object)
}

function Get-HsTestSummary($Tests, $Preparations = @()) {
    $byTarget = @{}
    foreach ($preparation in @($Preparations)) { $byTarget[[string]$preparation.target] = $preparation }
    foreach ($testMetadata in @($Tests)) {
        $target = "test_prep/$($testMetadata.name)"
        [pscustomobject][ordered]@{
            name = [string]$testMetadata.name
            labels = @(Get-HsCTestProperty $testMetadata 'LABELS')
            command = @($testMetadata.command)
            environment = @(Get-HsCTestProperty $testMetadata 'ENVIRONMENT')
            preparation = $byTarget[$target]
        }
    }
}

function Get-HsWorkingTreePaths([string]$RepoRoot) {
    # NUL records preserve quoted/non-ASCII paths and both sides of renames.
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = (Get-Command git -ErrorAction Stop).Source
    $startInfo.Arguments = "-c safe.directory=`"$RepoRoot`" -C `"$RepoRoot`" status --porcelain=v1 -z --untracked-files=all"
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $process = [Diagnostics.Process]::Start($startInfo)
    try {
        $raw = $process.StandardOutput.ReadToEnd()
        $diagnostic = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "Git status failed: $diagnostic" }
    } finally { $process.Dispose() }
    $records = $raw -split "`0"
    $paths = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    for ($index = 0; $index -lt $records.Count; ++$index) {
        $record = $records[$index]
        if (!$record) { continue }
        if ($record.Length -lt 4) { throw 'Invalid Git porcelain record.' }
        [void]$paths.Add($record.Substring(3).Replace('\', '/'))
        if ($record.Substring(0, 2) -match '[RC]') {
            ++$index
            if ($index -ge $records.Count -or !$records[$index]) { throw 'Incomplete Git rename record.' }
            [void]$paths.Add($records[$index].Replace('\', '/'))
        }
    }
    return @($paths | Sort-Object)
}

function ConvertTo-HsSourcePath([string]$Value, [string]$RepoRoot, [string]$BuildRoot) {
    $valuePath = $Value.Trim('"').Replace('\', '/')
    try {
        $absolute = if ([IO.Path]::IsPathRooted($valuePath)) {
            [IO.Path]::GetFullPath($valuePath)
        } else { [IO.Path]::GetFullPath((Join-Path $BuildRoot $valuePath)) }
        $prefix = [IO.Path]::GetFullPath($RepoRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
        if ($absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            return $absolute.Substring($prefix.Length).Replace('\', '/')
        }
    } catch { }
    return $null
}

function Test-HsDocumentationPath([string]$Path) {
    # Text configuration/data (notably CMakeLists.txt) remains conservative.
    # A configured graph reference always takes precedence over this classification.
    return $Path -match '\.md$' -or
        $Path -match '^(Docs|Documentation)/.*\.txt$' -or
        $Path -match '(^|/)(README|LICENSE|CHANGELOG|CONTRIBUTING|COPYING|NOTICE)\.txt$'
}

function Get-HsAffectedTests([string]$RepoRoot, [string]$BuildRoot, $Metadata, [string]$Ninja,
                             [string[]]$ChangedPaths) {
    $ErrorActionPreference = 'Continue'
    $changed = if ($PSBoundParameters.ContainsKey('ChangedPaths')) { @($ChangedPaths) } else { @(Get-HsWorkingTreePaths $RepoRoot) }
    $inputTests = @{}
    $queryFailures = @()
    foreach ($testMetadata in @($Metadata.tests)) {
        $name = [string]$testMetadata.name
        $inputs = @(& $Ninja -C $BuildRoot -t inputs "test_prep/$name" 2>&1)
        if ($LASTEXITCODE -ne 0) { $queryFailures += $name; continue }
        # Command paths cover script tests whose scripts need no build target.
        foreach ($inputValue in @($inputs) + @($testMetadata.command)) {
            $path = ConvertTo-HsSourcePath ([string]$inputValue) $RepoRoot $BuildRoot
            if (!$path) { continue }
            if (!$inputTests.ContainsKey($path)) { $inputTests[$path] = @() }
            if ($inputTests[$path] -cnotcontains $name) { $inputTests[$path] += $name }
        }
    }
    $unknown = @()
    $ignored = @()
    $ignoredReasons = @()
    $reasons = @()
    $selected = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($path in $changed) {
        $normalized = $path.Replace('\', '/')
        if (!$inputTests.ContainsKey($normalized)) {
            if (Test-HsDocumentationPath $normalized) {
                $ignored += $path
                $ignoredReasons += [ordered]@{
                    path = $path
                    reason = 'documentation-only path has no configured Ninja input or CTest command reference'
                }
            } else { $unknown += $path }
            continue
        }
        $names = @($inputTests[$normalized] | Sort-Object)
        foreach ($name in $names) { [void]$selected.Add($name) }
        $reasons += [ordered]@{ path = $path; tests = $names; authority = 'configured-ninja-inputs-or-ctest-command' }
    }
    $expanded = $unknown.Count -gt 0 -or $queryFailures.Count -gt 0
    if ($expanded) { foreach ($testMetadata in @($Metadata.tests)) { [void]$selected.Add([string]$testMetadata.name) } }
    $selectedTests = @(Select-HsExactTests $Metadata @($selected | Sort-Object))
    $labels = @($selectedTests | ForEach-Object { Get-HsCTestProperty $_ 'LABELS' } | Sort-Object -Unique)
    return [pscustomobject][ordered]@{
        schema_version = 1
        changed_paths = @($changed)
        tests = @($selected | Sort-Object)
        labels = $labels
        selected_tests = @(Get-HsTestSummary $selectedTests)
        unknown_paths = @($unknown)
        ignored_paths = @($ignored)
        ignored_reasons = @($ignoredReasons)
        query_failures = @($queryFailures)
        reasons = @($reasons)
        expanded = [bool]$expanded
        validation_scope = $(if ($expanded) { 'all-configured-tests' } else { 'configured-dependency-planning' })
        full_gate_required = $true
        note = 'Conservative development planning from configured metadata. Missing or unknown dependencies expand selection; this does not establish exact action correctness.'
    }
}
