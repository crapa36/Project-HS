[CmdletBinding()]
param(
    [string]$BuildRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) 'Build/msvc-validation')
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $repoRoot 'tools/verification_metadata.ps1')
$cache = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $BuildRoot 'CMakeCache.txt')) {
    if ($line -match '^([^#/:][^:]*):[^=]+=(.*)$') { $cache[$matches[1]] = $matches[2] }
}
$ctest = Join-Path (Split-Path $cache.CMAKE_COMMAND -Parent) 'ctest.exe'
$ninja = [string]$cache.CMAKE_MAKE_PROGRAM
$metadata = Get-HsCTestMetadata $ctest $BuildRoot

function Assert([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw "Verification selection contract: $Message" }
}

# A name containing regex syntax still selects only that exact configured name.
$fixture = [pscustomobject]@{ tests = @(
    [pscustomobject]@{ name = 'literal[.]test' },
    [pscustomobject]@{ name = 'literal.test' }
) }
$exact = @(Select-HsExactTests $fixture @('literal[.]test', 'literal.test', 'literal[.]test'))
Assert ($exact.Count -eq 2 -and $exact[0].name -ceq 'literal[.]test' -and
        $exact[1].name -ceq 'literal.test') 'exact lists must preserve order and remove duplicates'
$rejected = $false
try { Select-HsExactTests $fixture @('literal.*') | Out-Null } catch { $rejected = $true }
Assert $rejected 'unknown names must fail instead of expanding a regex'

$names = @($metadata.tests | ForEach-Object { [string]$_.name })
Assert ($names.Count -gt 0) 'configured CTest metadata must contain tests'
$queryCache = @{}
$preparations = @(
    foreach ($testMetadata in $metadata.tests) {
        Get-HsTestPreparation $ninja $BuildRoot ([string]$testMetadata.name) $queryCache
    }
)
Assert ($preparations.Count -eq $names.Count) 'every configured test needs a Ninja prep target'
$scopes = @(Get-HsGenerationScopes $metadata.tests $preparations)
foreach ($preparation in $preparations) {
    foreach ($scope in $preparation.generation_scopes) {
        Assert ($scopes -ccontains $scope) 'prerequisite Generation scopes must feed repair selection'
    }
}

$labelName = [string](@($metadata.tests | ForEach-Object { Get-HsCTestProperty $_ 'LABELS' } | Sort-Object -Unique)[0])
Assert (![string]::IsNullOrWhiteSpace($labelName)) 'configured tests must expose labels'
$labelTests = @((Get-HsCTestMetadata -CTest $ctest -BuildRoot $BuildRoot -Labels @($labelName)).tests)
$expectedLabelNames = @($metadata.tests | Where-Object {
    @(Get-HsCTestProperty $_ 'LABELS') -ccontains $labelName
} | ForEach-Object { [string]$_.name } | Sort-Object)
$actualLabelNames = @($labelTests | ForEach-Object { [string]$_.name } | Sort-Object)
Assert ([string]::Join("`n", $expectedLabelNames) -ceq [string]::Join("`n", $actualLabelNames)) 'label selection must match actual CTest labels'

$unchanged = Get-HsAffectedTests -RepoRoot $repoRoot -BuildRoot $BuildRoot -Metadata $metadata -Ninja $ninja -ChangedPaths @()
Assert (@($unchanged.tests).Count -eq 0 -and !$unchanged.expanded) 'an empty changed list must select no tests'
$documentation = Get-HsAffectedTests -RepoRoot $repoRoot -BuildRoot $BuildRoot -Metadata $metadata -Ninja $ninja -ChangedPaths @('Docs/__unreferenced_documentation__.md', 'Docs/__unreferenced_documentation__.txt')
Assert (@($documentation.tests).Count -eq 0 -and !$documentation.expanded -and
        @($documentation.ignored_paths).Count -eq 2 -and @($documentation.ignored_reasons).Count -eq 2) 'unreferenced documentation must select no tests and explain why'
$referencedDocumentation = [pscustomobject]@{ tests = @([pscustomobject]@{
    name = $names[0]
    command = @((Join-Path $repoRoot 'Docs/__referenced_documentation__.md'))
    properties = @()
}) }
$referenced = Get-HsAffectedTests -RepoRoot $repoRoot -BuildRoot $BuildRoot -Metadata $referencedDocumentation -Ninja $ninja -ChangedPaths @('Docs/__referenced_documentation__.md')
Assert (!$referenced.expanded -and @($referenced.tests).Count -eq 1 -and
        @($referenced.ignored_paths).Count -eq 0) 'configured references must override documentation exclusion'
$unknown = Get-HsAffectedTests -RepoRoot $repoRoot -BuildRoot $BuildRoot -Metadata $metadata -Ninja $ninja -ChangedPaths @('Source/__undeclared_dependency__.hpp', '__unreferenced_config__/CMakeLists.txt', '.codex/__unreferenced_config__.toml')
Assert ($unknown.expanded -and @($unknown.tests).Count -eq $names.Count) 'unknown dependencies must expand to all configured tests'
Assert (@($unknown.unknown_paths).Count -eq 3 -and @($unknown.ignored_paths).Count -eq 0) 'unknown source/config paths must remain conservative'
Assert (@($unknown.labels).Count -gt 0 -and @($unknown.selected_tests).Count -eq $names.Count) 'affected planning must expose actual test names and labels'
Assert $unknown.full_gate_required 'affected planning must preserve the merge/full gate policy'

$scriptTest = @($metadata.tests | Where-Object {
    @($_.command | Where-Object { [string]$_ -match '[/\\]Tests[/\\].+\.ps1$' }).Count -gt 0
}) | Select-Object -First 1
if ($scriptTest) {
    $commandPath = @($scriptTest.command | Where-Object { [string]$_ -match '[/\\]Tests[/\\].+\.ps1$' })[0]
    $sourcePath = ConvertTo-HsSourcePath ([string]$commandPath) $repoRoot $BuildRoot
    $affectedScript = Get-HsAffectedTests -RepoRoot $repoRoot -BuildRoot $BuildRoot -Metadata $metadata -Ninja $ninja -ChangedPaths @($sourcePath)
    Assert (!$affectedScript.expanded -and @($affectedScript.tests) -ccontains ([string]$scriptTest.name)) 'CTest script paths must participate in affected planning'
}

Write-Output "Verification selection contract passed: tests=$($names.Count) labels=$(@($unknown.labels).Count) generation_scopes=$($scopes.Count)"
