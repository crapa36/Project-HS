[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CMake,
    [Parameter(Mandatory = $true)][string]$Ninja
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $repoRoot 'tools/verification_metadata.ps1')
$ctest = Join-Path (Split-Path $CMake -Parent) 'ctest.exe'
$temporaryRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'Build')) + [IO.Path]::DirectorySeparatorChar
$fixtureRoot = Join-Path $temporaryRoot ('hs-selection-' + [guid]::NewGuid().ToString('N'))
$buildRoot = Join-Path $fixtureRoot 'build'

function Assert([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw "Verification selection contract: $Message" }
}

try {
    New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
    @'
cmake_minimum_required(VERSION 4.4)
project(SelectionFixture NONE)
set(CMAKE_TEST_BUILD_DEPENDS ON)
enable_testing()
add_custom_target(prepare_fixture COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/prepared")
add_custom_target(unrelated COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/unrelated")
add_test(NAME setup COMMAND "${CMAKE_COMMAND}" -E true BUILD_DEPENDS prepare_fixture)
set_tests_properties(setup PROPERTIES FIXTURES_SETUP sample)
add_test(NAME "literal.test" COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_SOURCE_DIR}/check.cmake")
set_tests_properties("literal.test" PROPERTIES FIXTURES_REQUIRED sample LABELS "label[.]" TIMEOUT 3)
add_test(NAME "literalXtest" COMMAND "${CMAKE_COMMAND}" -E false)
set_tests_properties("literalXtest" PROPERTIES LABELS "label.")
add_test(NAME cleanup COMMAND "${CMAKE_COMMAND}" -E true)
set_tests_properties(cleanup PROPERTIES FIXTURES_CLEANUP sample)
'@ | Set-Content (Join-Path $fixtureRoot 'CMakeLists.txt')
    'if(NOT EXISTS "${CMAKE_BINARY_DIR}/prepared")', 'message(FATAL_ERROR "fixture prerequisite was not built")', 'endif()' |
        Set-Content (Join-Path $fixtureRoot 'check.cmake')
    & $CMake -S $fixtureRoot -B $buildRoot -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" | Out-Null
    Assert ($LASTEXITCODE -eq 0) 'fixture configure failed'
    $selectionFile = Join-Path $fixtureRoot 'selected.txt'
    $requested = @('literal.test', 'literal.test')
    [IO.File]::WriteAllLines($selectionFile, [string[]]$requested, [Text.UTF8Encoding]::new($false))
    $metadata = Get-HsCTestMetadata -CTest $ctest -BuildRoot $buildRoot -TestNamesFile $selectionFile
    $exact = @(Select-HsExactTests $metadata $requested)
    Assert ($exact.Count -eq 1 -and $exact[0].name -ceq 'literal.test') 'exact selection must deduplicate without regex expansion'
    $selected = @($metadata.tests)
    $selectedNames = @($selected | ForEach-Object { $_.name } | Sort-Object)
    Assert (($selectedNames -join ',') -ceq 'cleanup,literal.test,setup') 'CTest must expand required fixture setup and cleanup'
    $invalidFile = Join-Path $fixtureRoot 'invalid.txt'
    foreach ($invalid in @(@{ Names = @('literal.*') }, @{ Names = @('literal.test', 'literal.*') })) {
        [IO.File]::WriteAllLines($invalidFile, [string[]]$invalid.Names, [Text.UTF8Encoding]::new($false))
        $invalidMetadata = Get-HsCTestMetadata -CTest $ctest -BuildRoot $buildRoot -TestNamesFile $invalidFile
        $rejected = $false
        try { Select-HsExactTests $invalidMetadata $invalid.Names | Out-Null } catch { $rejected = $true }
        Assert $rejected 'unknown literal names must fail even when mixed with valid tests and expanded fixtures'
    }
    $labelSelection = @((Get-HsCTestMetadata -CTest $ctest -BuildRoot $buildRoot -Labels @('label[.]')).tests)
    Assert ((@($labelSelection | ForEach-Object { $_.name } | Sort-Object) -join ',') -ceq ($selectedNames -join ',')) 'label selection must remain literal and include fixtures'
    $targets = @($selected | ForEach-Object { "test_prep/$($_.name)" })
    & $CMake --build $buildRoot --target @targets
    Assert ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $buildRoot 'prepared'))) 'native prep must build the fixture dependency'
    Assert (!(Test-Path (Join-Path $buildRoot 'unrelated'))) 'partial preparation must not build unrelated targets'
    & $ctest --test-dir $buildRoot --tests-from-file $selectionFile --no-tests=error --output-on-failure
    Assert ($LASTEXITCODE -eq 0) 'selected fixture execution failed'
    try {
        # Windows PowerShell converts native stderr to errors; this failure is intentional.
        $ErrorActionPreference = 'Continue'
        & $ctest --test-dir $buildRoot -R '^literalXtest$' --no-tests=error --output-on-failure *> (Join-Path $fixtureRoot 'failure.log')
        $failureExitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = 'Stop' }
    Assert ($failureExitCode -ne 0) 'native test failure must remain a failure'
    Write-Output 'Exact selection, labels, fixture prerequisite partial builds, and native failure contracts passed.'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    if (!$resolved.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notmatch '^hs-selection-[0-9a-f]{32}$') {
        throw 'Refusing to remove an unexpected fixture path'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
