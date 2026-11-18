[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CMakeArguments
)

$ErrorActionPreference = 'Stop'
if ($CMakeArguments.Count -ge 2 -and $CMakeArguments[0] -eq '--build') {
    $buildRoot = $CMakeArguments[1]
    if ($buildRoot -eq '--preset') {
        $preset = $CMakeArguments[2]
        $tree = if ($preset -eq 'verify') { 'msvc-debug' } else { $preset }
        $buildRoot = Join-Path $PSScriptRoot "../Build/$tree"
    }
    if (Test-Path -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt')) {
        . (Join-Path $PSScriptRoot 'msvc_environment.ps1')
        [void](Initialize-HsMsvcEnvironment -BuildRoot $buildRoot)
    }
}
$cmake = (& (Join-Path $PSScriptRoot 'bootstrap_cmake.ps1') -PrintPath | Select-Object -Last 1)
if (!$cmake -or !(Test-Path -LiteralPath $cmake -PathType Leaf)) {
    throw 'Repo-local CMake bootstrap did not return a valid executable.'
}
# Windows PowerShell wraps native stderr (including CMake warnings) as error
# records. The native exit code remains the authority for configure/build.
$ErrorActionPreference = 'Continue'
& $cmake @CMakeArguments
exit $LASTEXITCODE
