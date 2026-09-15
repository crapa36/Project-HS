[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Binary,
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'
$binaryPath = (Resolve-Path -LiteralPath $Binary).Path
$rootPath = (Resolve-Path -LiteralPath $SourceRoot).Path
$buildRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'Build'
$work = Join-Path $buildRoot ('combat-suite-input-regression-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null

function Write-JsonFile([string]$name, $value) {
    $path = Join-Path $work $name
    $value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding utf8
    return $path
}

function Invoke-Validation([string]$name, [string]$suite, [string]$expected, [bool]$shouldPass) {
    $outputDirectory = Join-Path $work ($name + '-output')
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = (& $binaryPath "--relic-balance-suite=$suite" "--output=$outputDirectory" '--validate-only' 2>&1 | Out-String).Trim()
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    $exitCode = $LASTEXITCODE
    $matched = $output.Contains($expected)
    if (($shouldPass -and ($exitCode -ne 0 -or -not $matched)) -or
        (-not $shouldPass -and ($exitCode -eq 0 -or -not $matched))) {
        throw "${name}: unexpected validation result (exit=$exitCode, expected='$expected', output='$output')"
    }
    if (Test-Path -LiteralPath $outputDirectory) {
        throw "${name}: validate-only created output directory '$outputDirectory'"
    }
    Write-Host "passed $name"
}

function Invoke-OutputGuard([string]$suite) {
    $outputDirectory = Join-Path $work 'existing-output'
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    $marker = Join-Path $outputDirectory 'combat_report.json'
    Set-Content -LiteralPath $marker -Value 'keep' -Encoding utf8
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = (& $binaryPath "--relic-balance-suite=$suite" "--output=$outputDirectory" 2>&1 | Out-String).Trim()
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($LASTEXITCODE -eq 0 -or -not $output.Contains('Relic balance output already exists')) {
        throw "existing-output: overwrite guard failed (exit=$LASTEXITCODE, output='$output')"
    }
    if ((Get-Content -LiteralPath $marker -Raw).Trim() -ne 'keep') {
        throw 'existing-output: existing report was modified'
    }
    Write-Host 'passed existing-output'
}

try {
    $valid = Write-JsonFile 'valid.json' ([ordered]@{
        schema_version = 1; duration_seconds = 1; seeds = @(1001)
        cases = @([ordered]@{
            id = 'valid-case'; scenario = 'movement'; skills = @()
            upgrades = [ordered]@{}; relic = 'pickup_reward'; expect_activation = $false
        })
    })
    Invoke-Validation 'positive' $valid 'validated relic_balance_suite planned_builds=2 pairs=1 seeds=1 ticks=60' $true
    Invoke-OutputGuard $valid

    $experimentRoot = Join-Path $rootPath 'ContentSource\Experiments'
    foreach ($checkedInSuite in Get-ChildItem -LiteralPath $experimentRoot -Filter 'relic_balance*.json') {
        Invoke-Validation ('checked-in-' + $checkedInSuite.BaseName) $checkedInSuite.FullName 'validated relic_balance_suite' $true
    }

    $unknownRoot = Write-JsonFile 'unknown-root.json' ([ordered]@{
        schema_version = 1; duration_seconds = 1; seeds = @(1001); cases = @(); unexpected = $true
    })
    Invoke-Validation 'unknown-root' $unknownRoot 'Relic balance suite key is invalid: unexpected' $false

    $badScenario = Write-JsonFile 'bad-scenario.json' ([ordered]@{
        schema_version = 1; duration_seconds = 1; seeds = @(1001)
        cases = @([ordered]@{ id = 'bad'; scenario = 'typo'; skills = @(); upgrades = [ordered]@{}; relic = 'pickup_reward' })
    })
    Invoke-Validation 'bad-scenario' $badScenario 'Relic balance case scenario is invalid: typo' $false

    $bothUpgrades = Write-JsonFile 'both-upgrades.json' ([ordered]@{
        schema_version = 1; duration_seconds = 1; seeds = @(1001)
        cases = @([ordered]@{ id = 'bad'; skills = @(); upgrades = [ordered]@{}; upgrade_masks = [ordered]@{}; relic = 'pickup_reward' })
    })
    Invoke-Validation 'both-upgrades' $bothUpgrades 'require exactly one of upgrades or upgrade_masks' $false

    $neitherMode = Write-JsonFile 'neither-mode.json' ([ordered]@{ schema_version = 1; duration_seconds = 1; seeds = @(1001) })
    Invoke-Validation 'neither-mode' $neitherMode 'requires exactly one of cases or a positive pair_limit' $false

    $zeroLimit = Write-JsonFile 'zero-limit.json' ([ordered]@{ schema_version = 1; duration_seconds = 1; seeds = @(1001); pair_limit = 0 })
    Invoke-Validation 'zero-pair-limit' $zeroLimit 'pair_limit must be in [1, 12600]' $false

    $negativeLimit = Write-JsonFile 'negative-limit.json' ([ordered]@{ schema_version = 1; duration_seconds = 1; seeds = @(1001); pair_limit = -1 })
    Invoke-Validation 'negative-pair-limit' $negativeLimit 'pair_limit must be a positive integer' $false

    $fractionalLimit = Write-JsonFile 'fractional-limit.json' ([ordered]@{ schema_version = 1; duration_seconds = 1; seeds = @(1001); pair_limit = 1.5 })
    Invoke-Validation 'fractional-pair-limit' $fractionalLimit 'pair_limit must be a positive integer' $false

    $largeDuration = Write-JsonFile 'large-duration.json' ([ordered]@{ schema_version = 1; duration_seconds = 4294967297; seeds = @(1001); pair_limit = 1 })
    Invoke-Validation 'large-duration' $largeDuration 'duration_seconds must be in [1, 1800]' $false

    $emptyCases = Write-JsonFile 'empty-cases.json' ([ordered]@{ schema_version = 1; duration_seconds = 1; seeds = @(1001); cases = @() })
    Invoke-Validation 'empty-cases' $emptyCases 'At least one focused relic case is required' $false
}
finally {
    $resolvedWork = [System.IO.Path]::GetFullPath($work)
    $resolvedBuildRoot = [System.IO.Path]::GetFullPath($buildRoot).TrimEnd('\') + '\'
    if ($resolvedWork.StartsWith($resolvedBuildRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedWork)) {
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force -ErrorAction SilentlyContinue
    }
}
