[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CMakeArguments
)

$ErrorActionPreference = 'Stop'
$cmake = (& (Join-Path $PSScriptRoot 'bootstrap_cmake.ps1') -PrintPath | Select-Object -Last 1)
if (!$cmake -or !(Test-Path -LiteralPath $cmake -PathType Leaf)) {
    throw 'Repo-local CMake bootstrap did not return a valid executable.'
}
# Windows PowerShell wraps native stderr (including CMake warnings) as error
# records. The native exit code remains the authority for configure/build.
$ErrorActionPreference = 'Continue'
& $cmake @CMakeArguments
exit $LASTEXITCODE
