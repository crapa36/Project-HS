[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GenerationExecutable,
    [Parameter(Mandatory = $true)][string]$BuildRoot,
    [Parameter(Mandatory = $true)][string]$SourceRoot
)

$ErrorActionPreference = 'Stop'
$buildRootFull = [IO.Path]::GetFullPath($BuildRoot)
$sourceRootFull = [IO.Path]::GetFullPath($SourceRoot)
$expectedSource = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (![StringComparer]::OrdinalIgnoreCase.Equals($sourceRootFull, $expectedSource) -or
    !$buildRootFull.StartsWith($sourceRootFull + [IO.Path]::DirectorySeparatorChar,
                               [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Audio Generation check requires this repository and its contained build directory.'
}

function Invoke-GenerationVerify {
    $ErrorActionPreference = 'Continue'
    $output = (& $GenerationExecutable --verify --build-root $buildRootFull --scope test-audio 2>&1 |
        ForEach-Object { $_.ToString() } | Out-String).Trim()
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    return [pscustomobject]@{ ExitCode = $exitCode; Output = $output }
}

$check = Invoke-GenerationVerify
if ($check.ExitCode -eq 0) {
    Write-Host $check.Output
    exit 0
}
try {
    if (!$check.Output.StartsWith('{') -or !$check.Output.EndsWith('}')) {
        throw 'Expected one diagnostic object.'
    }
    $diagnostic = $check.Output | ConvertFrom-Json -ErrorAction Stop
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
        !$diagnostic.reason.StartsWith('audio/', [StringComparison]::Ordinal) -or
        $diagnostic.reason.IndexOf(': ', [StringComparison]::Ordinal) -le 'audio/'.Length -or
        [string]::IsNullOrWhiteSpace($diagnostic.reason.Substring(
            $diagnostic.reason.IndexOf(': ', [StringComparison]::Ordinal) + 2)) -or
        $diagnostic.scope -cne 'test-audio' -or
        ![StringComparer]::OrdinalIgnoreCase.Equals($diagnostic.build_root, $buildRootFull) -or
        $diagnostic.verify -isnot [bool] -or !$diagnostic.verify) {
        throw 'Diagnostic does not identify a repairable Audio product in test-audio.'
    }
} catch {
    throw "Audio Generation verification failed without a repairable product error: $($check.Output)"
}

$actionResult = [IO.Path]::GetFullPath((Join-Path $buildRootFull 'Actions/audio/result.json'))
$cursor = $actionResult
while ($cursor.StartsWith($sourceRootFull + [IO.Path]::DirectorySeparatorChar,
                          [StringComparison]::OrdinalIgnoreCase)) {
    $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
    if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to repair through a reparse point: $cursor"
    }
    $cursor = Split-Path $cursor -Parent
}
if (!(Test-Path -LiteralPath $actionResult -PathType Leaf)) {
    throw "Cannot invalidate missing Audio ActionResult: $actionResult"
}
Write-Host $check.Output
Write-Host 'Repairing audio via CMake/Ninja after invalidating its stable ActionResult.'
Remove-Item -LiteralPath $actionResult -Force

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer/vswhere.exe is required for repair.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or !$installation) { throw 'MSVC x64 build tools were not found.' }
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$environmentLines = & $env:ComSpec /d /c "`"$developerCommand`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio developer environment initialization failed.' }
$environmentNames = @{}
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$' -and !$environmentNames.ContainsKey($matches[1])) {
        $environmentNames[$matches[1]] = $true
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$cmake = (& (Join-Path $expectedSource 'tools/bootstrap_cmake.ps1') -PrintPath | Select-Object -Last 1)
if (!$cmake -or !(Test-Path -LiteralPath $cmake -PathType Leaf)) {
    throw 'Repo-local CMake bootstrap did not return a valid executable.'
}
& $cmake --build $buildRootFull --target hs_audio_generation
if ($LASTEXITCODE -ne 0) { throw 'CMake/Ninja could not repair the audio Generation.' }

$check = Invoke-GenerationVerify
if ($check.ExitCode -ne 0) { throw "Audio Generation remains invalid after repair: $($check.Output)" }
Write-Host $check.Output
