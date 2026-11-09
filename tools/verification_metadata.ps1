# Shared, read-only views of the configured CTest/Ninja authorities.
# CTest selects tests and fixtures; Ninja resolves explicit build aliases.
function Get-HsCTestMetadata([string]$CTest, [string]$BuildRoot, [string[]]$Labels = @(), [string]$Preset, [string]$TestNamesFile) {
    $ErrorActionPreference = 'Continue'
    $arguments = if ($Preset) { @('--preset', $Preset) } else { @('--test-dir', $BuildRoot) }
    foreach ($label in $Labels) {
        if ([string]::IsNullOrWhiteSpace($label) -or $label -match '[\r\n]') {
            throw 'Labels must be nonempty CTest label names.'
        }
        # CTest's label selector is a regex; quote the requested label literally.
        $arguments += @('--label-regex', ('^' + [regex]::Escape($label) + '$'))
    }
    if ($TestNamesFile) { $arguments += @('--tests-from-file', $TestNamesFile) }
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
