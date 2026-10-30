[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('build', 'verify')]
    [string]$Command,
    [Parameter(Position = 1)]
    [string]$Node,
    [Parameter(Position = 2)]
    [string]$Target,
    [string]$BuildRoot,
    [ValidateSet('verify-core', 'verify')]
    [string]$Workflow = 'verify-core',
    [Alias('Tests')]
    [string[]]$Test,
    [string[]]$Label,
    [switch]$All,
    [switch]$DryRun,
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
    if (!$Json -and $DryRun) {
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

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $repoRoot 'Build/msvc-validation'
}
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if ($Command -eq 'build') {
    if ($Node -or $All -or
        $PSBoundParameters.ContainsKey('Test') -or $PSBoundParameters.ContainsKey('Label') -or
        $PSBoundParameters.ContainsKey('Workflow')) {
        Fail 'build accepts only -Target, -BuildRoot, -DryRun, and -Json'
    }
    Invoke-ExplicitBuild
    if ($DryRun) { return }
    exit [int]$script:HsDevBuildExitCode
}
if ($Command -eq 'verify') {
    if ($Target -or $DryRun -or $PSBoundParameters.ContainsKey('BuildRoot')) {
        Fail 'verify accepts an exact test, -Test, -Label, -All, and -Workflow'
    }
    if (!$PSBoundParameters.ContainsKey('Workflow')) { $Workflow = 'verify' }
    $verifyArguments = @{ Workflow = $Workflow; Json = [bool]$Json }
    if ($Node -and $PSBoundParameters.ContainsKey('Test')) { Fail 'verify accepts either a positional test or -Test' }
    if ($Node) { $verifyArguments.Test = @($Node) }
    elseif ($PSBoundParameters.ContainsKey('Test')) { $verifyArguments.Test = $Test }
    if ($PSBoundParameters.ContainsKey('Label')) { $verifyArguments.Label = $Label }
    if ($All) { $verifyArguments.All = $true }
    & (Join-Path $PSScriptRoot 'verify.ps1') @verifyArguments
    exit $LASTEXITCODE
}
