[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('doctor', 'status', 'build', 'verify', 'why', 'deps', 'rdeps', 'path', 'affected', 'gc')]
    [string]$Command,
    [Parameter(Position = 1)]
    [string]$Node,
    [Parameter(Position = 2)]
    [string]$Target,
    [string]$BuildRoot,
    [string]$GenerationPointer,
    [Alias('working-tree')]
    [switch]$WorkingTree,
    [ValidateSet('verify-core', 'verify')]
    [string]$Workflow = 'verify-core',
    [Alias('Tests')]
    [string[]]$Test,
    [string[]]$Label,
    [switch]$Changed,
    [switch]$All,
    [switch]$DryRun,
    [switch]$Apply,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'verification_metadata.ps1')

function Fail([string]$Message, [string]$Code = 'HS_DEV_FAILURE', [string]$Dependency = $null,
    [string]$Remediation = 'Correct the reported input or prerequisite and rerun this command.') {
    if ($Json) {
        $diagnostic = [ordered]@{
            schema_version = 1
            code = $Code
            command = [string]$Command
            node = if ($Node) { [string]$Node } else { $null }
            target = if ($Target) { [string]$Target } else { $null }
            build_root = [string]$BuildRoot
            test = @(if ($Test) { $Test })
            reason = [string]$Message
            dependency = if ($Dependency) { [string]$Dependency } else { $null }
            remediation = if ($Remediation) { [string]$Remediation } else { $null }
        }
        [Console]::Error.WriteLine(($diagnostic | ConvertTo-Json -Depth 8 -Compress))
        exit 1
    }
    throw "hs_dev: $Message"
}

function Normalize-Id([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return '' }
    $normalized = $Value.Trim().Replace('\', '/')
    while ($normalized.StartsWith('./', [StringComparison]::Ordinal)) {
        $normalized = $normalized.Substring(2)
    }
    return $normalized
}

function Read-Json([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail "metadata file does not exist: $Path"
    }
    try {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    } catch {
        Fail "invalid JSON metadata: $Path ($($_.Exception.Message))"
    }
}

function Read-CacheValues([string]$Path) {
    $values = @{}
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $Path) {
            if ($line -match '^([^#/:][^:]*):[^=]+=(.*)$') {
                $values[$matches[1]] = $matches[2]
            }
        }
    }
    return $values
}

function Find-Executable([string[]]$Candidates) {
    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $command = Get-Command $candidate -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command -and $command.Source -and
            (Test-Path -LiteralPath $command.Source -PathType Leaf)) {
            return $command.Source
        }
    }
    return $null
}

function Write-Doctor {
    $cache = Read-CacheValues (Join-Path $BuildRoot 'CMakeCache.txt')
    $presets = Read-Json (Join-Path $repoRoot 'CMakePresets.json')
    $requiredCMake = [version]"$($presets.cmakeMinimumRequired.major).$($presets.cmakeMinimumRequired.minor).$($presets.cmakeMinimumRequired.patch)"
    $cmakeCandidates = @()
    $buildConfig = Join-Path $BuildRoot 'hs_build_config.json'
    if (Test-Path -LiteralPath $buildConfig -PathType Leaf) {
        $cmakeCandidates += [string](Read-Json $buildConfig).cmake_executable
    }
    $cmakeCandidates += @([string]$cache.CMAKE_COMMAND, 'cmake.exe')
    $cmake = Find-Executable $cmakeCandidates
    $cmakeVersion = $null
    if ($cmake) {
        $versionLines = @(& $cmake --version 2>$null)
        if ($LASTEXITCODE -eq 0 -and $versionLines.Count -gt 0 -and
            $versionLines[0] -match '^cmake version (\d+\.\d+\.\d+)') {
            $cmakeVersion = $matches[1]
        }
    }
    $vswherePath = if (${env:ProgramFiles(x86)}) {
        Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    } else { $null }
    $vswhere = Find-Executable @($vswherePath)
    $installation = $null
    if ($vswhere) {
        $installations = @(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null)
        if ($LASTEXITCODE -eq 0 -and $installations.Count -gt 0) {
            $installation = [string]$installations[0]
        }
    }
    $compiler = Find-Executable @([string]$cache.CMAKE_CXX_COMPILER, 'cl.exe')
    $bundledNinja = if ($installation) {
        Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
    } else { $null }
    $ninja = Find-Executable @('ninja.exe', $bundledNinja)
    $sdk = Find-Executable @([string]$cache.CMAKE_RC_COMPILER, 'rc.exe')
    $dxc = Find-Executable @([string]$cache.HS_DXC, 'dxc.exe')
    $fbxRoot = [string]$cache.HS_FBX_SDK_ROOT
    if (!$fbxRoot) { $fbxRoot = 'C:/Program Files/Autodesk/FBX/FBX SDK/2020.3.7' }
    $fbxHeader = Join-Path $fbxRoot 'include/fbxsdk.h'
    $fbxLibrary = Join-Path $fbxRoot 'lib/x64/release/libfbxsdk-mt.lib'
    $audioTool = Find-Executable @((Join-Path $BuildRoot 'hs_audio_content.exe'))
    $gpuAdapter = @()
    $gpuProbe = 'unavailable'
    if (Get-Command Get-CimInstance -ErrorAction SilentlyContinue) {
        try {
            $gpuAdapter = @(Get-CimInstance Win32_VideoController -ErrorAction Stop |
                Where-Object { $_.Name -and $_.Name -notmatch 'Basic Display' } |
                Select-Object -ExpandProperty Name)
            $gpuProbe = 'completed'
        } catch { $gpuProbe = 'failed' }
    }
    $gpuAvailable = if ($gpuProbe -eq 'completed') { [bool]($gpuAdapter.Count -gt 0) } else { $null }
    $debugLayer = Test-Path -LiteralPath (Join-Path $env:WINDIR 'System32/d3d12sdklayers.dll') -PathType Leaf
    $capabilities = [ordered]@{
        cmake = @{ available = [bool]($cmakeVersion -and ([version]$cmakeVersion -ge $requiredCMake)); path = $cmake; version = $cmakeVersion; required_version = [string]$requiredCMake; required_for = @('verify-core', 'verify') }
        ninja = @{ available = [bool]$ninja; path = $ninja; required_for = @('verify-core', 'verify') }
        msvc = @{ available = [bool]($installation -and $compiler); path = $compiler; installation = [string]$installation; required_for = @('verify-core', 'verify') }
        windows_sdk = @{ available = [bool]$sdk; path = $sdk; required_for = @('verify-core', 'verify') }
        dxc = @{ available = [bool]$dxc; path = $dxc; required_for = @('verify', 'hs_shaders') }
        fbx_sdk = @{ available = [bool]((Test-Path -LiteralPath $fbxHeader -PathType Leaf) -and (Test-Path -LiteralPath $fbxLibrary -PathType Leaf)); path = $fbxRoot; required_for = @('verify', 'hs_character_content') }
        gpu = @{ available = $gpuAvailable; adapters = @($gpuAdapter); probe = $gpuProbe; validation_layer_present = [bool]$debugLayer; runtime_validation = 'not-tested'; required_for = @('integration.render-smoke', 'msvc-gpu-validation') }
        audio_tooling = @{ available = [bool]$audioTool; path = $audioTool; kind = 'repository-hs_audio_content'; required_for = @('hs_audio_generation', 'content.audio-*') }
    }
    [ordered]@{ schema_version = 1; command = 'doctor'; build_root = $BuildRoot; capabilities = $capabilities } |
        ConvertTo-Json -Depth 7
}

function Resolve-BuildLocator([string]$Locator, [string]$Context) {
    if ([string]::IsNullOrWhiteSpace($Locator)) {
        Fail "empty locator in $Context"
    }
    $root = $BuildRoot
    while ($root.EndsWith('\') -or $root.EndsWith('/')) {
        $root = $root.Substring(0, $root.Length - 1)
    }
    $rootPrefix = $root + [IO.Path]::DirectorySeparatorChar
    $resolved = [IO.Path]::GetFullPath((Join-Path $BuildRoot $Locator))
    if (!$resolved.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        Fail "locator escapes BuildRoot in $Context`: $Locator"
    }
    return $resolved
}

function Get-ActionNamespaceFromPath([string]$Path) {
    $actionsRoot = [IO.Path]::GetFullPath((Join-Path $BuildRoot 'Actions')).TrimEnd('\', '/')
    $rootPrefix = $actionsRoot + [IO.Path]::DirectorySeparatorChar
    $resolved = [IO.Path]::GetFullPath($Path)
    if (!$resolved.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        Fail "ActionResult path escapes Actions root: $Path"
    }
    $relative = $resolved.Substring($rootPrefix.Length).Replace('\', '/')
    $parts = @($relative -split '/')
    if ($parts.Count -lt 2 -or $parts[$parts.Count - 1] -cne 'result.json') {
        Fail "invalid ActionResult path under Actions: $Path"
    }
    $namespaceParts = @($parts[0..($parts.Count - 2)])
    if (@($namespaceParts | Where-Object { !$_ -or $_ -eq '.' -or $_ -eq '..' -or
                $_ -notmatch '^[A-Za-z0-9._-]{1,128}$' }).Count -gt 0) {
        Fail "invalid ActionResult namespace path: $Path"
    }
    return [string]::Join('/', $namespaceParts)
}

function Get-AllActionResultFiles {
    $actionsRoot = [IO.Path]::GetFullPath((Join-Path $BuildRoot 'Actions'))
    if (!(Test-Path -LiteralPath $actionsRoot -PathType Container)) {
        Fail "Actions directory does not exist: $actionsRoot"
    }
    $files = @(Get-ChildItem -LiteralPath $actionsRoot -Recurse -Filter 'result.json' -File |
        Sort-Object FullName)
    if ($files.Count -eq 0) { Fail "no ActionResults under Actions: $actionsRoot" }
    $entries = foreach ($file in $files) {
        [pscustomobject]@{
            File = $file
            Namespace = Get-ActionNamespaceFromPath $file.FullName
        }
    }
    return @($entries)
}

function Validate-ActionId([string]$ActionId, [string]$Context) {
    if ([string]::IsNullOrWhiteSpace($ActionId) -or $ActionId.Length -gt 128 -or
        $ActionId -notmatch '^[A-Za-z0-9._-]+$') {
        Fail "invalid action_id in $Context"
    }
}

function Get-BuildActionTargets {
    $targets = @{}
    $actionsRoot = Join-Path $BuildRoot 'Actions'
    if (!(Test-Path -LiteralPath $actionsRoot -PathType Container)) { return $targets }
    foreach ($file in @(Get-ChildItem -LiteralPath $actionsRoot -Recurse -Filter 'result.json' -File |
            Sort-Object FullName)) {
        $action = Read-Json $file.FullName
        $actionId = [string]$action.action_id
        Validate-ActionId $actionId $file.FullName
        if ($action.schema_version -ne 1 -or $null -eq $action.inputs -or $null -eq $action.products) {
            Fail "ActionResult lacks inputs/products: $($file.FullName)"
        }
        $namespace = Get-ActionNamespaceFromPath $file.FullName
        $declaredNamespace = [string]$action.namespace_path
        if ([string]::IsNullOrWhiteSpace($declaredNamespace)) {
            if ($namespace -cne 'audio' -or $actionId -cne 'audio') {
                Fail "ActionResult namespace_path is missing outside legacy audio: $($file.FullName)"
            }
        } elseif ((Normalize-Id $declaredNamespace) -cne $namespace) {
            Fail "ActionResult namespace mismatch: $($file.FullName)"
        }
        if ($targets.ContainsKey($actionId)) { Fail "duplicate action_id '$actionId': $($file.FullName)" }
        $target = "Actions/$namespace/result.json"
        [void](Resolve-BuildLocator $target "build action $actionId")
        $targets[$actionId] = [pscustomobject]@{ Id = $actionId; Target = $target }
    }
    return $targets
}

function Invoke-ExplicitBuild {
    if ([string]::IsNullOrWhiteSpace($Target) -or $Target -match '[\r\n]') {
        Fail 'build requires a nonempty comma-separated -Target list'
    }
    $requested = @($Target.Split(',') | ForEach-Object { $_.Trim() })
    foreach ($name in $requested) {
        if ($name -notmatch '^[A-Za-z0-9_][A-Za-z0-9_.+-]*$') {
            Fail "invalid explicit build target or Action ID: $name"
        }
        if ($name -in @('all', 'clean', 'test', 'install', 'help', 'edit_cache', 'rebuild_cache')) {
            Fail "build requires named native targets or Action IDs, not '$name'"
        }
    }
    $cache = Read-CacheValues (Join-Path $BuildRoot 'CMakeCache.txt')
    $ninja = [string]$cache.CMAKE_MAKE_PROGRAM
    if ($cache.CMAKE_GENERATOR -cne 'Ninja' -or
        !(Test-Path -LiteralPath (Join-Path $BuildRoot 'build.ninja') -PathType Leaf) -or
        !$ninja -or !(Test-Path -LiteralPath $ninja -PathType Leaf)) {
        Fail 'build requires a configured CMake/Ninja build tree'
    }
    $queryCache = @{}
    $actionTargets = $null
    $resolved = [System.Collections.Generic.List[string]]::new()
    $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $resolutions = @()
    foreach ($name in $requested) {
        $query = $null
        try { $query = Get-HsNinjaQuery $ninja $BuildRoot $name $queryCache } catch { }
        if ($query -and $query.rule -ceq 'phony') {
            $resolvedTarget = $name
            $kind = 'native'
        } else {
            if ($null -eq $actionTargets) { $actionTargets = Get-BuildActionTargets }
            if (!$actionTargets.ContainsKey($name)) { Fail "unknown native build target or Action ID: $name" }
            $resolvedTarget = [string]$actionTargets[$name].Target
            try { $query = Get-HsNinjaQuery $ninja $BuildRoot $resolvedTarget $queryCache }
            catch {
                if ($Json) {
                    Fail "ActionResult target is absent from the configured Ninja graph: $resolvedTarget" `
                        -Code 'HS_DEV_TARGET_UNAVAILABLE' -Dependency $resolvedTarget `
                        -Remediation 'Configure this build tree for the requested Action and retry.'
                }
                throw
            }
            if ($query.rule -cne 'CUSTOM_COMMAND') {
                Fail "ActionResult is not a configured Ninja action output: $resolvedTarget"
            }
            $kind = 'action'
        }
        if ($seen.Add($resolvedTarget)) { $resolved.Add($resolvedTarget) }
        $resolutions += [ordered]@{ requested = $name; kind = $kind; target = $resolvedTarget }
    }
    $wrapper = Join-Path $PSScriptRoot 'cmake.ps1'
    $arguments = @('--build', $BuildRoot, '--target') + $resolved.ToArray()
    $plan = [ordered]@{
        schema_version = 1; command = 'build'; build_root = $BuildRoot; dry_run = [bool]$DryRun
        requested_targets = $requested; resolved_targets = $resolved.ToArray(); resolutions = $resolutions
        authority = 'configured-ninja-query-and-actionresults'; cmake_wrapper = $wrapper
        cmake_arguments = $arguments
    }
    if ($Json -and $DryRun) {
        $plan | ConvertTo-Json -Depth 6
        $script:HsDevBuildExitCode = 0
        return
    }
    if (!$Json) {
        Write-Output "build dry_run=$($DryRun.IsPresent.ToString().ToLowerInvariant()) build_root=$BuildRoot"
        foreach ($resolution in $resolutions) {
            Write-Output "requested=$($resolution.requested) kind=$($resolution.kind) target=$($resolution.target)"
        }
        Write-Output "cmake_wrapper=$wrapper"
        Write-Output ('cmake_arguments=' + (ConvertTo-Json -InputObject $arguments -Compress))
    }
    if ($DryRun) {
        $script:HsDevBuildExitCode = 0
        return
    }
    if ($Json) {
        $nativeErrorPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            & $wrapper -CMakeArguments $arguments 2>&1 |
                ForEach-Object { [Console]::Error.WriteLine([string]$_) }
            $exitCode = [int]$LASTEXITCODE
        } catch {
            Fail "build invocation failed: $($_.Exception.Message)"
        } finally {
            $ErrorActionPreference = $nativeErrorPreference
        }
        $plan.exit_code = $exitCode
        $plan.succeeded = ($exitCode -eq 0)
        $plan | ConvertTo-Json -Depth 8
        $script:HsDevBuildExitCode = $exitCode
        return
    }
    & $wrapper -CMakeArguments $arguments
    $script:HsDevBuildExitCode = [int]$LASTEXITCODE
}

function Add-KnownNode([string]$Value) {
    $id = Normalize-Id $Value
    if ($id) { $knownNodes[$id] = $true }
}

function Add-Edge([string]$Dependency, [string]$Dependent) {
    $dependency = Normalize-Id $Dependency
    $dependent = Normalize-Id $Dependent
    if (!$dependency -or !$dependent) { Fail 'empty dependency edge in ActionResult metadata' }
    Add-KnownNode $dependency
    Add-KnownNode $dependent
    if (!$dependencies.ContainsKey($dependent)) { $dependencies[$dependent] = @() }
    if ($dependencies[$dependent] -notcontains $dependency) {
        $dependencies[$dependent] = @($dependencies[$dependent]) + $dependency
    }
    if (!$reverseDependencies.ContainsKey($dependency)) { $reverseDependencies[$dependency] = @() }
    if ($reverseDependencies[$dependency] -notcontains $dependent) {
        $reverseDependencies[$dependency] = @($reverseDependencies[$dependency]) + $dependent
    }
}

function Resolve-Node([string]$Value) {
    $id = Normalize-Id $Value
    foreach ($known in $knownNodes.Keys) {
        if ([StringComparer]::OrdinalIgnoreCase.Equals($known, $id)) { return $known }
    }
    Fail "unknown dependency node: $Value"
}

function Sorted-Edges([hashtable]$Table, [string]$Key) {
    if (!$Table.ContainsKey($Key)) { return @() }
    return @($Table[$Key] | Sort-Object)
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $repoRoot 'Build/msvc-validation'
}
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if ($Command -eq 'build') {
    if ($Node -or $WorkingTree -or $Changed -or $All -or $Apply -or $GenerationPointer -or
        $PSBoundParameters.ContainsKey('Test') -or $PSBoundParameters.ContainsKey('Label') -or
        $PSBoundParameters.ContainsKey('Workflow')) {
        Fail 'build accepts only -Target, -BuildRoot, -DryRun, and -Json'
    }
    Invoke-ExplicitBuild
    if ($DryRun) { return }
    exit [int]$script:HsDevBuildExitCode
}
if ($Command -eq 'doctor') {
    Write-Doctor
    return
}
if ($Command -eq 'verify') {
    if ($Target -or $DryRun -or $Apply -or $WorkingTree) {
        Fail 'verify accepts an exact test, -Test, -Label, -Changed, -All, and -Workflow'
    }
    if (!$PSBoundParameters.ContainsKey('Workflow')) { $Workflow = 'verify' }
    $verifyArguments = @{ Workflow = $Workflow; Json = [bool]$Json }
    if ($Node -and $PSBoundParameters.ContainsKey('Test')) { Fail 'verify accepts either a positional test or -Test' }
    if ($Node) { $verifyArguments.Test = @($Node) }
    elseif ($PSBoundParameters.ContainsKey('Test')) { $verifyArguments.Test = $Test }
    if ($PSBoundParameters.ContainsKey('Label')) { $verifyArguments.Label = $Label }
    if ($Changed) { $verifyArguments.Changed = $true }
    if ($All) { $verifyArguments.All = $true }
    & (Join-Path $PSScriptRoot 'verify.ps1') @verifyArguments
    exit $LASTEXITCODE
}
if ($Command -eq 'affected') {
    if (!$WorkingTree -and $Node -ne '--working-tree') { Fail 'affected requires --working-tree' }
    $cache = Read-CacheValues (Join-Path $BuildRoot 'CMakeCache.txt')
    $cmakePath = [string]$cache.CMAKE_COMMAND
    $ctestPath = if ($cmakePath) { Join-Path (Split-Path $cmakePath -Parent) 'ctest.exe' } else { $null }
    $ninjaPath = [string]$cache.CMAKE_MAKE_PROGRAM
    if (!$ctestPath -or !(Test-Path -LiteralPath $ctestPath -PathType Leaf) -or
        !$ninjaPath -or !(Test-Path -LiteralPath $ninjaPath -PathType Leaf)) {
        Fail 'affected requires a configured CTest/Ninja build tree; run verify to configure it'
    }
    $metadata = Get-HsCTestMetadata $ctestPath $BuildRoot
    $analysis = Get-HsAffectedTests $repoRoot $BuildRoot $metadata $ninjaPath
    $result = [ordered]@{ schema_version = 2; command = 'affected'; build_root = $BuildRoot }
    foreach ($property in $analysis.PSObject.Properties) {
        if ($property.Name -ne 'schema_version') { $result[$property.Name] = $property.Value }
    }
    if ($Json) { $result | ConvertTo-Json -Depth 12 }
    else {
        Write-Output "affected mode=working-tree validation_scope=$($analysis.validation_scope)"
        foreach ($name in $analysis.tests) { Write-Output "test=$name" }
        foreach ($labelName in $analysis.labels) { Write-Output "label=$labelName" }
        foreach ($path in $analysis.unknown_paths) { Write-Output "unknown=$path" }
        foreach ($ignored in $analysis.ignored_reasons) { Write-Output "ignored=$($ignored.path) reason=$($ignored.reason)" }
        Write-Output 'authority=configured-ninja-inputs-or-ctest-command'
        Write-Output 'purpose=conservative-development-planning'
        Write-Output 'full_gate_required=true'
    }
    return
}
if (!(Test-Path -LiteralPath $BuildRoot -PathType Container)) {
    Fail "build root does not exist: $BuildRoot"
}
if ([string]::IsNullOrWhiteSpace($GenerationPointer)) {
    $GenerationPointer = Join-Path $BuildRoot 'GenerationState/runtime-full.json'
}
$GenerationPointer = [IO.Path]::GetFullPath($GenerationPointer)
$queryAllActions = @('why', 'deps', 'rdeps') -contains $Command
$selectedMetadataAvailable = Test-Path -LiteralPath $GenerationPointer -PathType Leaf
if (!$selectedMetadataAvailable -and !$queryAllActions) {
    Fail "Generation pointer does not exist: $GenerationPointer"
}

$pointer = $null
$manifestPath = $null
$manifest = $null
$selectedProducts = @{}
if ($selectedMetadataAvailable) {
    $pointer = Read-Json $GenerationPointer
    if ($pointer.schema_version -ne 1 -or
        [string]::IsNullOrWhiteSpace([string]$pointer.generation_id) -or
        [string]::IsNullOrWhiteSpace([string]$pointer.scope) -or
        [string]::IsNullOrWhiteSpace([string]$pointer.manifest_locator)) {
        Fail "invalid Generation pointer: $GenerationPointer"
    }
    $manifestPath = Resolve-BuildLocator ([string]$pointer.manifest_locator) 'Generation pointer'
    $manifest = Read-Json $manifestPath
    if ($manifest.schema_version -notin @(2, 3) -or
        [string]$manifest.generation_id -ne [string]$pointer.generation_id -or
        [string]$manifest.scope -ne [string]$pointer.scope -or
        $null -eq $manifest.products -or $null -eq $manifest.required_ids -or
        $null -eq $manifest.upstream_product_dependencies) {
        Fail "Generation manifest does not match pointer: $manifestPath"
    }

    $manifestIds = @()
    foreach ($product in @($manifest.products)) {
    $logicalId = Normalize-Id ([string]$product.logical_id)
    $locator = [string]$product.locator
    if (!$logicalId -or !$locator -or $selectedProducts.ContainsKey($logicalId)) {
        Fail "invalid or duplicate Generation product: $manifestPath"
    }
    if ($manifest.schema_version -eq 3) {
        $hasher = [Security.Cryptography.SHA256]::Create()
        try {
            $nameHash = ([BitConverter]::ToString($hasher.ComputeHash(
                [Text.Encoding]::UTF8.GetBytes([string]$product.logical_id)))).Replace('-', '').ToLowerInvariant()
        } finally { $hasher.Dispose() }
        if ($locator -cne "Generations/$($pointer.generation_id)/products/$nameHash.bin") {
            Fail "invalid snapshot locator for $logicalId"
        }
    }
    $productPath = Resolve-BuildLocator $locator "Generation product $logicalId"
    if (!(Test-Path -LiteralPath $productPath -PathType Leaf)) {
        Fail "selected Generation product is missing: $logicalId -> $productPath"
    }
    $selectedProducts[$logicalId] = [pscustomobject]@{
        Path = $productPath
        Action = Normalize-Id ([string]$product.action_id)
        Locator = $locator
        Namespace = [string]$product.namespace_path
        Digest = [string]$product.digest
        Size = [string]$product.size
    }
    $manifestIds += $logicalId
    }
    $manifestIds = @($manifestIds)
    $requiredIds = @($manifest.required_ids | ForEach-Object { Normalize-Id ([string]$_) })
    if ($manifestIds.Count -ne @($manifestIds | Sort-Object -Unique).Count -or
        $requiredIds.Count -ne $manifestIds.Count) {
        Fail "Generation required_ids do not match products: $manifestPath"
    }
    for ($index = 0; $index -lt $manifestIds.Count; ++$index) {
        if ($index -gt 0 -and
            [StringComparer]::Ordinal.Compare($manifestIds[$index - 1], $manifestIds[$index]) -gt 0) {
            Fail "Generation products are not sorted: $manifestPath"
        }
        if ($requiredIds[$index] -ne $manifestIds[$index]) {
            Fail "Generation required_ids do not match products: $manifestPath"
        }
    }
}

$actionById = @{}
$actionFileById = @{}
$producerByProduct = @{}
$productDependencies = @{}
$dependencies = @{}
$reverseDependencies = @{}
$knownNodes = @{}
$inputActions = @{}
$activeActionFiles = @{}
$activeActionIds = @{}
$activeActionIdByFile = @{}
foreach ($selected in $selectedProducts.Values) {
    if ($manifest.schema_version -eq 3) {
        $namespace = [string]$selected.Namespace
        if ($namespace -cnotmatch '^[A-Za-z0-9._-]{1,128}(/[A-Za-z0-9._-]{1,128})*$' -or
            @($namespace -split '/' | Where-Object { $_ -eq '.' -or $_ -eq '..' }).Count -gt 0) {
            Fail "invalid selected Generation action namespace: $namespace"
        }
    } else {
        $parts = (Normalize-Id ([string]$selected.Locator)) -split '/'
        if ($parts.Count -lt 4 -or $parts[0] -cne 'Products' -or
            $parts[$parts.Count - 2] -cnotmatch '^[0-9a-f]{64}$' -or
            @($parts | Where-Object { !$_ -or $_ -eq '.' -or $_ -eq '..' }).Count -gt 0) {
            Fail "invalid selected Generation product locator: $($selected.Locator)"
        }
        $namespace = [string]::Join('/', $parts[1..($parts.Count - 3)])
    }
    $actionFile = Resolve-BuildLocator "Actions/$namespace/result.json" 'selected Generation action'
    if (!(Test-Path -LiteralPath $actionFile -PathType Leaf)) {
        Fail "selected Generation ActionResult is missing: $actionFile"
    }
    $activeActionFiles[$actionFile] = $true
    if ($selected.Action) {
        if ($activeActionIds.ContainsKey($selected.Action) -and
            $activeActionIds[$selected.Action] -ne $actionFile) {
            Fail "selected Generation action has multiple namespaces: $($selected.Action)"
        }
        if ($activeActionIdByFile.ContainsKey($actionFile) -and
            $activeActionIdByFile[$actionFile] -ne $selected.Action) {
            Fail "selected Generation namespace has multiple action IDs: $actionFile"
        }
        $activeActionIds[$selected.Action] = $actionFile
        $activeActionIdByFile[$actionFile] = $selected.Action
    }
}
$actionFiles = @($activeActionFiles.Keys | Sort-Object | ForEach-Object {
    Get-Item -LiteralPath $_
})
if ($actionFiles.Count -eq 0 -and !$queryAllActions) { Fail 'selected Generation has no ActionResult metadata' }
if ($queryAllActions) {
    $actionFiles = @(Get-AllActionResultFiles | ForEach-Object { $_.File })
}

foreach ($actionFile in $actionFiles) {
    $action = Read-Json $actionFile.FullName
    $actionId = Normalize-Id ([string]$action.action_id)
    Validate-ActionId $actionId $actionFile.FullName
    if ($actionById.ContainsKey($actionId)) {
        Fail "duplicate action_id '$actionId': $($actionFile.FullName)"
    }
    if ($action.schema_version -ne 1 -or $null -eq $action.inputs -or $null -eq $action.products) {
        Fail "ActionResult lacks inputs/products: $($actionFile.FullName)"
    }
    $actionNamespace = Get-ActionNamespaceFromPath $actionFile.FullName
    $hasNamespacePath = $action.PSObject.Properties.Name -contains 'namespace_path'
    $declaredNamespace = if ($hasNamespacePath) { $action.namespace_path } else { $null }
    if ($null -eq $declaredNamespace -or [string]::IsNullOrWhiteSpace([string]$declaredNamespace)) {
        if ($actionNamespace -cne 'audio' -or $actionId -cne 'audio') {
            $reason = if ($hasNamespacePath) { 'null' } else { 'missing' }
            Fail "ActionResult namespace_path is $reason outside legacy audio: $($actionFile.FullName)"
        }
    } elseif ((Normalize-Id ([string]$declaredNamespace)) -cne $actionNamespace) {
        Fail "ActionResult namespace mismatch: $($actionFile.FullName)"
    }
    if ($activeActionIdByFile.ContainsKey($actionFile.FullName) -and
        $activeActionIdByFile[$actionFile.FullName] -ne $actionId) {
        Fail "selected Generation action ID mismatches ActionResult: $($actionFile.FullName)"
    }
    $actionById[$actionId] = $action
    $actionFileById[$actionId] = $actionFile.FullName
    Add-KnownNode $actionId

    foreach ($input in @($action.inputs)) {
        $inputId = Normalize-Id ([string]$input.logical_id)
        if (!$inputId) { Fail "ActionResult input lacks logical_id: $($actionFile.FullName)" }
        Add-Edge $inputId $actionId
        if (!$inputActions.ContainsKey($inputId)) { $inputActions[$inputId] = @() }
        if ($inputActions[$inputId] -notcontains $actionId) {
            $inputActions[$inputId] = @($inputActions[$inputId]) + $actionId
        }
    }
    foreach ($upstream in @($action.upstream_product_dependencies)) {
        $upstreamId = Normalize-Id ([string]$upstream.logical_id)
        if (!$upstreamId) { Fail "upstream dependency lacks logical_id: $($actionFile.FullName)" }
        Add-Edge $upstreamId $actionId
    }
    foreach ($product in @($action.products)) {
        $productId = Normalize-Id ([string]$product.logical_id)
        $productLocator = Normalize-Id ([string]$product.locator)
        if (!$productId -or !$productLocator -or $producerByProduct.ContainsKey($productId)) {
            Fail "invalid or duplicate product in ActionResult: $($actionFile.FullName)"
        }
        $productPath = Resolve-BuildLocator $productLocator "ActionResult product $productId"
        if (!(Test-Path -LiteralPath $productPath -PathType Leaf)) {
            Fail "ActionResult product is missing: $productId -> $productPath"
        }
        $producerByProduct[$productId] = $actionId
        $productDependencies[$productId] = @(
            if ($null -eq $product.dependencies) { @() } else { @($product.dependencies) }
        )
        Add-Edge $actionId $productId
    }
}

foreach ($productId in @($productDependencies.Keys | Sort-Object)) {
    foreach ($dependency in @($productDependencies[$productId])) {
        $dependencyId = Normalize-Id ([string]$dependency)
        if (!$dependencyId -or !$producerByProduct.ContainsKey($dependencyId)) {
            Fail "product dependency references unknown product: $productId -> $([string]$dependency)"
        }
        Add-Edge $dependencyId $productId
    }
}

foreach ($selectedId in $selectedProducts.Keys) {
    if (!$producerByProduct.ContainsKey($selectedId)) {
        Fail "Generation product has no matching ActionResult product: $selectedId"
    }
    if ($selectedProducts[$selectedId].Action -and
        $selectedProducts[$selectedId].Action -ne $producerByProduct[$selectedId]) {
        Fail "Generation product producer mismatch: $selectedId"
    }
}

function Write-ActionWhy([string]$ActionId) {
    if (!$actionById.ContainsKey($ActionId)) { Fail "unknown action: $ActionId" }
    $action = $actionById[$ActionId]
    $selectedGenerationAction = $activeActionIds.ContainsKey($ActionId)
    if ($Json) {
        $latestReceipt = Get-LatestActionReceipt $ActionId
        $inputs = @(
            foreach ($input in @($action.inputs | Sort-Object logical_id)) {
                [ordered]@{
                    id = [string]$input.logical_id
                    digest = [string]$input.digest
                }
            }
        )
        $upstream = @(
            foreach ($dependency in @($action.upstream_product_dependencies | Sort-Object logical_id)) {
                [ordered]@{
                    id = [string]$dependency.logical_id
                    digest = [string]$dependency.digest
                    size = [int64]$dependency.size
                }
            }
        )
        $products = @(
            foreach ($product in @($action.products | Sort-Object logical_id)) {
                $id = Normalize-Id ([string]$product.logical_id)
                $selected = $selectedProducts.ContainsKey($id)
                [ordered]@{
                    id = $id
                    digest = [string]$product.digest
                    size = [int64]$product.size
                    selected = [bool]$selected
                    resolved_path = if ($selected) { [string]$selectedProducts[$id].Path } else { $null }
                }
            }
        )
        $receiptValue = $null
        if ($latestReceipt) {
            $receiptValue = [ordered]@{ path = [string]$latestReceipt.Path }
            foreach ($property in $latestReceipt.Receipt.PSObject.Properties) {
                $receiptValue[$property.Name] = $property.Value
            }
        }
        $result = [ordered]@{
            schema_version = 1
            command = 'why'
            action_id = $ActionId
            result_path = [string]$actionFileById[$ActionId]
            action_key = [string]$action.action_key
            recipe_id = [string]$action.recipe_id
            recipe_version = [int64]$action.recipe_version
            selected_generation_action = [bool]$selectedGenerationAction
            generation_scope = if ($selectedGenerationAction) { [string]$manifest.scope } else { $null }
            generation_id = if ($selectedGenerationAction) { [string]$manifest.generation_id } else { $null }
            inputs = $inputs
            upstream_product_dependencies = $upstream
            products = $products
            latest_receipt = $receiptValue
        }
        if ($action.PSObject.Properties.Name -contains 'recipe_identity') {
            $result.recipe_identity = [string]$action.recipe_identity
        }
        $result | ConvertTo-Json -Depth 16
        return
    }
    Write-Output "why action=$ActionId"
    Write-Output "action_result=$($actionFileById[$ActionId])"
    Write-Output "action_key=$([string]$action.action_key)"
    Write-Output "recipe_id=$([string]$action.recipe_id)"
    if ($action.PSObject.Properties.Name -contains 'recipe_identity') {
        Write-Output "recipe_identity=$([string]$action.recipe_identity)"
    }
    Write-Output "recipe_version=$([string]$action.recipe_version)"
    Write-Output "selected_generation_action=$($selectedGenerationAction.ToString().ToLowerInvariant())"
    if ($selectedGenerationAction) {
        Write-Output "generation_scope=$([string]$manifest.scope)"
        Write-Output "generation_id=$([string]$manifest.generation_id)"
    } else {
        Write-Output 'generation_scope=unselected'
        Write-Output 'generation_id=unselected'
    }
    $inputs = @($action.inputs | Sort-Object logical_id)
    Write-Output "inputs=$($inputs.Count)"
    foreach ($input in $inputs) {
        Write-Output "input=$([string]$input.logical_id) digest=$([string]$input.digest)"
    }
    $upstream = @($action.upstream_product_dependencies | Sort-Object logical_id)
    Write-Output "upstream_product_dependencies=$($upstream.Count)"
    foreach ($dependency in $upstream) {
        Write-Output "upstream=$([string]$dependency.logical_id) digest=$([string]$dependency.digest) size=$([string]$dependency.size)"
    }
    $products = @($action.products | Sort-Object logical_id)
    Write-Output "products=$($products.Count)"
    foreach ($product in $products) {
        $id = Normalize-Id ([string]$product.logical_id)
        $selected = $selectedProducts.ContainsKey($id)
        $path = if ($selected) { $selectedProducts[$id].Path } else { '' }
        $selectedText = if ($selected) { 'true' } else { 'false' }
        Write-Output "product=$id digest=$([string]$product.digest) size=$([string]$product.size) selected=$selectedText path=$path"
    }
    $latestReceipt = Get-LatestActionReceipt $ActionId
    Write-LatestActionReceipt $latestReceipt
}

function Format-ReceiptValue($Value) {
    if ($null -eq $Value) { return 'unavailable' }
    if ($Value -is [bool]) { return $Value.ToString().ToLowerInvariant() }
    return [string]$Value
}

function Get-ProducerFailureCode([object[]]$Lines) {
    foreach ($line in @($Lines)) {
        $text = ([string]$line).Trim()
        if ($text.StartsWith('{')) {
            try {
                $diagnostic = $text | ConvertFrom-Json
                if ($diagnostic.schema_version -eq 1 -and $diagnostic.error_code) {
                    return [string]$diagnostic.error_code
                }
            } catch { }
        }
        if ([string]$line -match 'generation\.error\s+code=([^\s]+)') {
            return [string]$matches[1]
        }
    }
    return $null
}

function Get-LatestActionReceipt([string]$ActionId) {
    $actionDirectory = Split-Path -Parent $actionFileById[$ActionId]
    $actionsRoot = [IO.Path]::GetFullPath((Join-Path $BuildRoot 'Actions')).TrimEnd('\', '/') +
        [IO.Path]::DirectorySeparatorChar
    $actionDirectory = [IO.Path]::GetFullPath($actionDirectory)
    if (!$actionDirectory.StartsWith($actionsRoot, [StringComparison]::OrdinalIgnoreCase)) {
        Fail "ActionResult directory escapes Actions root: $actionDirectory"
    }
    $namespace = $actionDirectory.Substring($actionsRoot.Length)
    $receiptDirectory = Join-Path (Join-Path $BuildRoot 'ActionReceipts') $namespace
    if (!(Test-Path -LiteralPath $receiptDirectory -PathType Container)) {
        return
    }
    try {
        $files = @(Get-ChildItem -LiteralPath $receiptDirectory -Filter '*.json' -File |
            Sort-Object Name -Descending)
    } catch {
        return
    }
    foreach ($file in $files) {
        try {
            $receipt = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
            if ($receipt.schema_version -ne 1 -or [string]$receipt.action_id -ne $ActionId -or
                $null -eq $receipt.semantic_delta -or !$receipt.outcome) { continue }
        } catch { continue }
        return [pscustomobject]@{ Path = $file.FullName; Receipt = $receipt }
    }
    return
}

function Write-LatestActionReceipt($LatestReceipt) {
    if (!$LatestReceipt) {
        Write-Output 'latest_receipt=unavailable'
        return
    }
    $receipt = $LatestReceipt.Receipt
    Write-Output "latest_receipt=$($LatestReceipt.Path)"
    Write-Output "receipt_outcome=$([string]$receipt.outcome)"
    Write-Output "receipt_action_key=$([string]$receipt.action_key)"
    Write-Output "receipt_product_set_id=$([string]$receipt.product_set_id)"
    Write-Output "receipt_previous_action_key=$(Format-ReceiptValue $receipt.previous_action_key)"
    Write-Output "receipt_previous_product_set_id=$(Format-ReceiptValue $receipt.previous_product_set_id)"
    Write-Output "receipt_started_utc=$([string]$receipt.started_utc)"
    Write-Output "receipt_ended_utc=$([string]$receipt.ended_utc)"
    Write-Output "receipt_elapsed_ms=$([string]$receipt.elapsed_ms)"
    Write-Output "receipt_scheduler_reason=$(Format-ReceiptValue $receipt.scheduler_reason)"
    Write-Output "receipt_semantic_delta_action_key_changed=$(Format-ReceiptValue $receipt.semantic_delta.action_key_changed)"
    Write-Output "receipt_semantic_delta_product_set_changed=$(Format-ReceiptValue $receipt.semantic_delta.product_set_changed)"
}

function Write-Edges([string]$Mode, [string]$Value) {
    $resolved = Resolve-Node $Value
    $table = if ($Mode -eq 'deps') { $dependencies } else { $reverseDependencies }
    $items = @(Sorted-Edges $table $resolved)
    if ($Json) {
        $result = [ordered]@{
            schema_version = 1
            command = $Mode
            node = $resolved
        }
        if ($Mode -eq 'deps') {
            $result.dependencies = @($items)
        } else {
            $result.dependents = @($items)
        }
        $result | ConvertTo-Json -Depth 8
        return
    }
    Write-Output "$Mode node=$resolved count=$($items.Count)"
    foreach ($item in $items) { Write-Output $item }
}

function Get-PathResult([string]$FromValue, [string]$ToValue) {
    $from = Resolve-Node $FromValue
    $to = Resolve-Node $ToValue
    $parents = @{}
    $visited = @{}
    $queue = New-Object System.Collections.Queue
    $queue.Enqueue($from)
    $visited[$from] = $true
    while ($queue.Count -gt 0) {
        $current = [string]$queue.Dequeue()
        if ($current -eq $to) { break }
        foreach ($next in (Sorted-Edges $reverseDependencies $current)) {
            if (!$visited.ContainsKey($next)) {
                $visited[$next] = $true
                $parents[$next] = $current
                $queue.Enqueue($next)
            }
        }
    }
    if (!$visited.ContainsKey($to)) {
        return [pscustomobject]@{
            From = $from
            To = $to
            Found = $false
            Path = @()
            SameBfs = $true
        }
    }
    $path = New-Object System.Collections.Generic.List[string]
    $cursor = $to
    while ($true) {
        $path.Insert(0, $cursor)
        if ($cursor -eq $from) { break }
        $cursor = $parents[$cursor]
    }
    $pathValues = @($path | ForEach-Object { [string]$_ })
    return [pscustomobject]@{
        From = $from
        To = $to
        Found = $true
        Path = $pathValues
        SameBfs = $true
    }
}

function Find-Path([string]$FromValue, [string]$ToValue) {
    $result = Get-PathResult $FromValue $ToValue
    if ($Json) {
        [ordered]@{
            schema_version = 1
            command = 'path'
            from = $result.From
            to = $result.To
            found = [bool]$result.Found
            path = @($result.Path)
            same_bfs = [bool]$result.SameBfs
        } | ConvertTo-Json -Depth 8
        return
    }
    if (!$result.Found) {
        Write-Output "path from=$($result.From) to=$($result.To) found=false"
        return
    }
    Write-Output "path from=$($result.From) to=$($result.To) found=true length=$(@($result.Path).Count)"
    Write-Output ([string]::Join(' -> ', @($result.Path)))
}

function Get-WorkingTreePaths {
    $lines = @(& git -C $repoRoot status --porcelain=v1 --untracked-files=all 2>&1)
    if ($LASTEXITCODE -ne 0) { Fail "git status failed: $([string]::Join(' ', $lines))" }
    $paths = @()
    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line) -or $line.Length -lt 4) { continue }
        $path = $line.Substring(3)
        if ($path -match ' -> ') { $path = $path.Substring($path.LastIndexOf(' -> ') + 4) }
        $path = $path.Trim('"')
        $normalized = Normalize-Id $path
        if ($normalized -and $paths -notcontains $normalized) { $paths += $normalized }
    }
    return @($paths | Sort-Object)
}

function Get-AffectedAnalysis {
    if (!$WorkingTree -and $Node -ne '--working-tree') { Fail 'affected requires --working-tree' }
    $changed = Get-WorkingTreePaths
    $mapped = @{}
    $unknown = @()
    $impacted = @{}
    foreach ($path in $changed) {
        $matches = @()
        foreach ($inputId in $inputActions.Keys) {
            if ([StringComparer]::OrdinalIgnoreCase.Equals((Normalize-Id $path), (Normalize-Id $inputId))) {
                $matches += @($inputActions[$inputId])
            }
        }
        if ($matches.Count -eq 0) {
            $unknown += $path
            continue
        }
        $mapped[$path] = @($matches | Sort-Object -Unique)
        foreach ($actionId in $mapped[$path]) { $impacted[$actionId] = $true }
    }
    $queue = New-Object System.Collections.Queue
    $directByAction = @{}
    foreach ($path in $changed) {
        if (!$mapped.ContainsKey($path)) { continue }
        foreach ($actionId in @($mapped[$path] | Sort-Object)) {
            if (!$directByAction.ContainsKey($actionId)) { $directByAction[$actionId] = @() }
            $directByAction[$actionId] = @($directByAction[$actionId]) + $path
        }
    }
    foreach ($actionId in @($directByAction.Keys | Sort-Object)) {
        foreach ($path in @($directByAction[$actionId] | Sort-Object -Unique)) {
            $queue.Enqueue([pscustomobject]@{
                Node = $actionId
                Path = @($path, $actionId)
            })
        }
    }
    $visited = @{}
    $reasonPaths = @{}
    while ($queue.Count -gt 0) {
        $entry = $queue.Dequeue()
        $current = [string]$entry.Node
        if ($visited.ContainsKey($current)) { continue }
        $visited[$current] = $true
        if ($actionById.ContainsKey($current) -and !$reasonPaths.ContainsKey($current)) {
            $reasonPaths[$current] = @($entry.Path)
        }
        foreach ($next in (Sorted-Edges $reverseDependencies $current)) {
            if ($actionById.ContainsKey($next) -and !$impacted.ContainsKey($next)) {
                $impacted[$next] = $true
            }
            if (!$visited.ContainsKey($next)) {
                $queue.Enqueue([pscustomobject]@{
                    Node = $next
                    Path = @($entry.Path) + $next
                })
            }
        }
    }
    $scope = if ($unknown.Count -gt 0) { 'broad-validation-required' } else { 'declared-dependency-scope' }
    return [pscustomobject]@{
        Changed = $changed
        Mapped = $mapped
        Unknown = $unknown
        Impacted = $impacted
        ReasonPaths = $reasonPaths
        Scope = $scope
    }
}

function Write-AffectedJson($analysis) {
    $changedFiles = @(
        foreach ($path in @($analysis.Changed)) {
            $directActions = if ($analysis.Mapped.ContainsKey($path)) {
                @($analysis.Mapped[$path] | Sort-Object -Unique)
            } else { @() }
            [ordered]@{
                path = $path
                direct_actions = $directActions
                known = [bool]$analysis.Mapped.ContainsKey($path)
            }
        }
    )
    $impactedActions = @(
        foreach ($actionId in @($analysis.Impacted.Keys | Sort-Object)) {
            [ordered]@{
                action_id = $actionId
                reason_path = @($analysis.ReasonPaths[$actionId])
            }
        }
    )
    [ordered]@{
        schema_version = 1
        command = 'affected'
        generation_scope = [string]$manifest.scope
        generation_id = [string]$manifest.generation_id
        changed_files = $changedFiles
        impacted_actions = $impactedActions
        validation_scope = [string]$analysis.Scope
        full_gate_required = $true
    } | ConvertTo-Json -Depth 12
}

function Write-Affected {
    $analysis = Get-AffectedAnalysis
    if ($Json) {
        Write-AffectedJson $analysis
        return
    }
    $changed = @($analysis.Changed)
    $mapped = $analysis.Mapped
    $unknown = @($analysis.Unknown)
    $impacted = $analysis.Impacted
    $scope = [string]$analysis.Scope
    Write-Output "affected mode=working-tree generation_scope=$($manifest.scope)"
    Write-Output "changed_files=$($changed.Count)"
    foreach ($path in $changed) {
        if ($mapped.ContainsKey($path)) {
            Write-Output "mapped=$path actions=$([string]::Join(',', @($mapped[$path])))"
        } else {
            Write-Output "unknown=$path"
        }
    }
    Write-Output "impacted_actions=$(@($impacted.Keys).Count)"
    foreach ($actionId in @($impacted.Keys | Sort-Object)) { Write-Output "action=$actionId" }
    Write-Output "validation_scope=$scope"
    Write-Output 'full_gate_required=true'
}

function Write-Status {
    $selectedActions = @($selectedProducts.Values |
        ForEach-Object { $_.Action } | Where-Object { $_ } | Sort-Object -Unique)
    $actionSummaries = @()
    $inputIds = @{}
    foreach ($actionId in $selectedActions) {
        $action = $actionById[$actionId]
        foreach ($input in @($action.inputs)) {
            $inputIds[(Normalize-Id ([string]$input.logical_id))] = $true
        }
        $actionSummaries += [ordered]@{
            action_id = $actionId
            result_path = $actionFileById[$actionId]
            action_key = [string]$action.action_key
            input_count = @($action.inputs).Count
        }
    }
    [ordered]@{
        schema_version = 1
        command = 'status'
        build_root = $BuildRoot
        generation_pointer = $GenerationPointer
        generation_manifest = $manifestPath
        generation_id = [string]$manifest.generation_id
        scope = [string]$manifest.scope
        selected_product_count = $selectedProducts.Count
        action_result_count = $actionSummaries.Count
        unique_input_count = $inputIds.Count
        action_results = $actionSummaries
    } | ConvertTo-Json -Depth 6
}

function Read-GcDryRunReport([string]$GenerationExecutable, [string]$GenerationId) {
    $arguments = @('--gc', '--dry-run', '--json', '--build-root', $BuildRoot,
        '--pin', $GenerationId)
    $lines = @(& $GenerationExecutable @arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        $failureCode = Get-ProducerFailureCode $lines
        if ($Json -and $failureCode) {
            Fail "generation gc dry-run failed: $([string]::Join([Environment]::NewLine, $lines))" $failureCode
        }
        Fail "generation gc dry-run failed: $([string]::Join([Environment]::NewLine, $lines))"
    }
    $text = [string]::Join([Environment]::NewLine, @($lines))
    if ([string]::IsNullOrWhiteSpace($text)) {
        Fail 'generation gc dry-run returned no report'
    }
    try {
        $report = $text | ConvertFrom-Json
    } catch {
        Fail "generation gc dry-run returned invalid JSON: $($_.Exception.Message)"
    }
    if ($null -eq $report -or $report -is [string] -or $report -is [ValueType]) {
        Fail 'generation gc dry-run returned a non-object report'
    }
    if ($report.PSObject.Properties.Name -notcontains 'schema_version' -or
        $report.schema_version -ne 1 -or
        $report.PSObject.Properties.Name -notcontains 'command' -or
        [string]$report.command -cne 'gc' -or
        $report.PSObject.Properties.Name -notcontains 'dry_run' -or
        !$report.dry_run) {
        Fail 'generation gc dry-run report schema mismatch'
    }
    if ($report.PSObject.Properties.Name -contains 'generation_id' -and
        [string]$report.generation_id -cne $GenerationId) {
        Fail 'generation gc dry-run report generation mismatch'
    }
    return $report
}

function Invoke-Gc {
    if ($DryRun -eq $Apply) { Fail 'gc requires exactly one of -DryRun or -Apply' }
    if ($Node -or $Target -or $WorkingTree) { Fail 'gc does not accept query arguments' }
    $expectedPointer = [IO.Path]::GetFullPath((Join-Path $BuildRoot "GenerationState/$($pointer.scope).json"))
    if (![StringComparer]::OrdinalIgnoreCase.Equals($GenerationPointer, $expectedPointer)) {
        Fail 'gc requires a selected GenerationState pointer within BuildRoot'
    }
    $generationExecutable = Join-Path $BuildRoot 'hs_generation.exe'
    if (!(Test-Path -LiteralPath $generationExecutable -PathType Leaf)) {
        Fail "generation executable does not exist: $generationExecutable"
    }
    # The producer's verifier checks product digests and pointer identity before GC.
    $verificationOutput = @(& $generationExecutable --verify --build-root $BuildRoot `
        --scope ([string]$pointer.scope) 2>&1)
    if ($LASTEXITCODE -ne 0) {
        $failureCode = Get-ProducerFailureCode $verificationOutput
        if ($Json -and $failureCode) {
            Fail "selected Generation verification failed; gc was not run: $([string]::Join([Environment]::NewLine, $verificationOutput))" $failureCode
        }
        Fail "selected Generation verification failed; gc was not run: $verificationOutput"
    }
    if ($DryRun) {
        $report = Read-GcDryRunReport $generationExecutable ([string]$pointer.generation_id)
        $result = [ordered]@{
            schema_version = 1
            command = 'gc'
            build_root = $BuildRoot
            generation_pointer = $GenerationPointer
            generation_id = [string]$pointer.generation_id
            dry_run = $true
            generation_report = $report
        }
        if ($Json) {
            $result | ConvertTo-Json -Depth 32
        } else {
            Write-Output "gc dry_run=true generation_id=$($pointer.generation_id) build_root=$BuildRoot"
            $report | ConvertTo-Json -Depth 32
        }
        $script:HsDevGcExitCode = 0
        return
    }
    # Pin the generation verified above even if its pointer changes during this run.
    if ($Json) {
        $nativeDiagnostics = @()
        try {
            # hs_generation currently supports JSON only for dry-run. Keep the
            # mutating producer's legacy diagnostics off hs_dev JSON stdout.
            & $generationExecutable --gc --build-root $BuildRoot --pin ([string]$pointer.generation_id) 2>&1 |
                ForEach-Object {
                    $line = [string]$_
                    $nativeDiagnostics += $line
                    [Console]::Error.WriteLine($line)
                }
            $exitCode = [int]$LASTEXITCODE
        } catch {
            $nativeDiagnostics += [string]$_.Exception.Message
            [Console]::Error.WriteLine([string]$_.Exception.Message)
            $exitCode = 1
        }
        $result = [ordered]@{
            schema_version = 1
            command = 'gc'
            build_root = $BuildRoot
            generation_pointer = $GenerationPointer
            generation_id = [string]$pointer.generation_id
            dry_run = $false
            exit_code = $exitCode
            succeeded = ($exitCode -eq 0)
        }
        $failureCode = Get-ProducerFailureCode $nativeDiagnostics
        if ($failureCode) { $result.error_code = $failureCode }
        $result | ConvertTo-Json -Depth 16
        $script:HsDevGcExitCode = $exitCode
        return
    }
    & $generationExecutable --gc --build-root $BuildRoot --pin ([string]$pointer.generation_id)
    $script:HsDevGcExitCode = [int]$LASTEXITCODE
    if ($script:HsDevGcExitCode -ne 0) { Fail 'generation gc failed' }
}

switch ($Command) {
    'status' { Write-Status }
    'gc' {
        Invoke-Gc
        exit [int]$script:HsDevGcExitCode
    }
    'why' {
        if (!$Node) { Fail 'why requires an action ID' }
        Write-ActionWhy (Resolve-Node $Node)
    }
    'deps' {
        if (!$Node) { Fail 'deps requires a node' }
        Write-Edges 'deps' $Node
    }
    'rdeps' {
        if (!$Node) { Fail 'rdeps requires a node' }
        Write-Edges 'rdeps' $Node
    }
    'path' {
        if (!$Node -or !$Target) { Fail 'path requires <from> <to>' }
        Find-Path $Node $Target
    }
    'affected' { Write-Affected }
}
