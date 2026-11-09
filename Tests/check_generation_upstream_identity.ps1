param(
    [Parameter(Mandatory = $true)][string]$GenerationExecutable,
    [Parameter(Mandatory = $true)][string]$FixtureParent
)

$ErrorActionPreference = 'Stop'
# Keep the nested immutable product paths below Windows MAX_PATH.
$fixtureRoot = Join-Path ([IO.Path]::GetFullPath($FixtureParent)) (
    'up-' + [IO.Path]::GetRandomFileName())
[IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)

function Write-Product([string]$locator, [string]$content) {
    $path = Join-Path $fixtureRoot ($locator.Replace('/', [IO.Path]::DirectorySeparatorChar))
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
    $bytes = $utf8.GetBytes($content)
    [IO.File]::WriteAllBytes($path, $bytes)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $digest = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    return [pscustomobject]@{
        locator = $locator
        digest = 'sha256:' + $digest
        size = $bytes.Length
    }
}

function Write-Result([string]$namespace, [object]$result) {
    $path = Join-Path $fixtureRoot ("Actions/$namespace/result.json".Replace('/', [IO.Path]::DirectorySeparatorChar))
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
    [IO.File]::WriteAllText($path, ($result | ConvertTo-Json -Depth 10), $utf8)
    return $path
}

function Invoke-Generation([string[]]$arguments) {
    $ErrorActionPreference = 'Continue'
    $output = (& $GenerationExecutable @arguments 2>&1 |
        ForEach-Object { $_.ToString() } | Out-String).Trim()
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    return [pscustomobject]@{ ExitCode = $exitCode; Output = $output }
}

try {
    $shaderId = 'render/shader/rock_0_scene.dxil'
    $shaderSetOld = 'a' * 64
    $shaderSetNew = 'b' * 64
    $packageSet = 'c' * 64
    $oldShader = Write-Product "Products/shader/$shaderSetOld/rock_0_scene.dxil" 'shader-v1'
    $package = Write-Product "Products/package/$packageSet/rock_0.rpkg" 'rock-package'
    $shaderResult = [ordered]@{
        schema_version = 1; action_id = 'shader.rock0'; namespace_path = 'shader'
        action_key = 'sha256:' + ('1' * 64); product_set_id = $shaderSetOld
        upstream_product_dependencies = @()
        products = @([ordered]@{
            logical_id = $shaderId; locator = $oldShader.locator
            digest = $oldShader.digest; size = $oldShader.size
        })
    }
    $packageResult = [ordered]@{
        schema_version = 1; action_id = 'package.rock0'; namespace_path = 'package'
        action_key = 'sha256:' + ('2' * 64); product_set_id = $packageSet
        upstream_product_dependencies = @([ordered]@{
            logical_id = $shaderId; digest = $oldShader.digest; size = $oldShader.size
        })
        products = @([ordered]@{
            logical_id = 'render/rock_0.rpkg'; locator = $package.locator
            digest = $package.digest; size = $package.size
            dependencies = @($shaderId)
        })
    }
    $shaderPath = Write-Result 'shader' $shaderResult
    $packagePath = Write-Result 'package' $packageResult
    $composeArgs = @('--compose', '--build-root', $fixtureRoot, '--scope', 'test-upstream',
                     '--result', $shaderPath, '--result', $packagePath)
    $baseline = Invoke-Generation $composeArgs
    if ($baseline.ExitCode -ne 0) { throw "Baseline compose failed: $($baseline.Output)" }
    $pointer = Get-Content (Join-Path $fixtureRoot 'GenerationState/test-upstream.json') -Raw | ConvertFrom-Json
    $manifest = Get-Content (Join-Path $fixtureRoot $pointer.manifest_locator) -Raw | ConvertFrom-Json
    if ($manifest.schema_version -ne 3 -or
        $manifest.upstream_product_dependencies.Count -ne 1 -or
        $manifest.upstream_product_dependencies[0].action_id -ne 'package.rock0' -or
        $manifest.upstream_product_dependencies[0].digest -ne $oldShader.digest) {
        throw 'Immutable Generation manifest omitted its upstream identity edge.'
    }
    $shaderOnly = Invoke-Generation @('--compose', '--build-root', $fixtureRoot,
        '--scope', 'test-shader-only', '--result', $shaderPath, '--result', $packagePath,
        '--require', $shaderId)
    if ($shaderOnly.ExitCode -ne 0) {
        throw "Shader-only compose failed: $($shaderOnly.Output)"
    }
    $shaderOnlyPointer = Get-Content (Join-Path $fixtureRoot 'GenerationState/test-shader-only.json') -Raw | ConvertFrom-Json
    $shaderOnlyManifest = Get-Content (Join-Path $fixtureRoot $shaderOnlyPointer.manifest_locator) -Raw | ConvertFrom-Json
    if ($shaderOnlyManifest.upstream_product_dependencies.Count -ne 0) {
        throw 'Unselected package upstream edge leaked into shader-only Generation.'
    }
    $shaderOnlyVerify = Invoke-Generation @('--verify', '--build-root', $fixtureRoot,
        '--scope', 'test-shader-only')
    if ($shaderOnlyVerify.ExitCode -ne 0) {
        throw "Shader-only Generation failed verification: $($shaderOnlyVerify.Output)"
    }

    $newShader = Write-Product "Products/shader/$shaderSetNew/rock_0_scene.dxil" 'shader-v2'
    $shaderResult.product_set_id = $shaderSetNew
    $shaderResult.products[0].locator = $newShader.locator
    $shaderResult.products[0].digest = $newShader.digest
    $shaderResult.products[0].size = $newShader.size
    $null = Write-Result 'shader' $shaderResult
    $mixed = Invoke-Generation $composeArgs
    try {
        if ($mixed.ExitCode -eq 0 -or
            !$mixed.Output.StartsWith('{') -or !$mixed.Output.EndsWith('}')) {
            throw 'Expected a failed compose diagnostic object.'
        }
        $diagnostic = $mixed.Output | ConvertFrom-Json -ErrorAction Stop
        $fields = @($diagnostic.PSObject.Properties.Name)
        $textFields = @('producer_id', 'error_code', 'action_id', 'reason', 'build_root', 'scope')
        if ($diagnostic -isnot [pscustomobject] -or
            @(@('schema_version', 'compose') + $textFields | Where-Object { $fields -cnotcontains $_ }).Count -ne 0 -or
            @($textFields | Where-Object { $diagnostic.PSObject.Properties[$_].Value -isnot [string] }).Count -ne 0 -or
            !($diagnostic.schema_version -is [int] -or $diagnostic.schema_version -is [long]) -or
            $diagnostic.schema_version -ne 1 -or
            $diagnostic.producer_id -cne 'hs_generation' -or
            $diagnostic.error_code -cne 'upstream-product-mismatch' -or
            $diagnostic.action_id -cne 'package.rock0' -or
            $diagnostic.reason -cne "selected upstream product identity differs: $shaderId" -or
            $diagnostic.scope -cne 'test-upstream' -or
            ![StringComparer]::OrdinalIgnoreCase.Equals($diagnostic.build_root, $fixtureRoot) -or
            $diagnostic.compose -isnot [bool] -or !$diagnostic.compose) {
            throw 'Diagnostic does not identify the stale package upstream edge.'
        }
    } catch {
        throw "Mixed upstream was not rejected: $($mixed.Output)"
    }
    $after = Get-Content (Join-Path $fixtureRoot 'GenerationState/test-upstream.json') -Raw | ConvertFrom-Json
    if ($after.generation_id -ne $pointer.generation_id) {
        throw 'Rejected compose changed the selected Generation pointer.'
    }
    $verify = Invoke-Generation @('--verify', '--build-root', $fixtureRoot, '--scope', 'test-upstream')
    if ($verify.ExitCode -ne 0) {
        throw "Immutable baseline Generation failed verification: $($verify.Output)"
    }

    $audioSet = 'd' * 64
    $audioProduct = Write-Product "Products/audio/$audioSet/audio_catalog.hsbin" 'audio-fixture'
    $audioResult = [ordered]@{
        schema_version = 1; action_id = 'audio'; namespace_path = 'audio'
        action_key = 'sha256:' + ('4' * 64); product_set_id = $audioSet
        upstream_product_dependencies = @()
        products = @([ordered]@{
            logical_id = 'audio/audio_catalog.hsbin'; locator = $audioProduct.locator
            digest = $audioProduct.digest; size = $audioProduct.size
        })
    }
    $audioResultPath = Write-Result 'audio' $audioResult
    $audioCompose = Invoke-Generation @('--compose', '--build-root', $fixtureRoot,
        '--scope', 'test-audio', '--result', $audioResultPath)
    if ($audioCompose.ExitCode -ne 0) {
        throw "Audio fixture compose failed: $($audioCompose.Output)"
    }
    $audioPointerPath = Join-Path $fixtureRoot 'GenerationState/test-audio.json'
    $audioPointer = Get-Content -LiteralPath $audioPointerPath -Raw | ConvertFrom-Json
    $audioManifestPath = Join-Path $fixtureRoot $audioPointer.manifest_locator
    $audioManifest = Get-Content -LiteralPath $audioManifestPath -Raw | ConvertFrom-Json
    $audioProductPath = Join-Path $fixtureRoot ($audioManifest.products[0].locator.Replace('/', [IO.Path]::DirectorySeparatorChar))
    $checkerPath = Join-Path $PSScriptRoot 'check_audio_generation.ps1'
    $childPowerShell = if ($PSVersionTable.PSEdition -eq 'Core') {
        Join-Path $PSHOME 'pwsh.exe'
    } else {
        Join-Path $PSHOME 'powershell.exe'
    }
    $generationExecutableFull = [IO.Path]::GetFullPath($GenerationExecutable)
    $sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $validOutput = (& $childPowerShell -NoProfile -File $checkerPath `
        -GenerationExecutable $generationExecutableFull -BuildRoot $fixtureRoot `
        -SourceRoot $sourceRoot 2>&1 | ForEach-Object { $_.ToString() } | Out-String).Trim()
    $validExit = $LASTEXITCODE
    if ($validExit -ne 0) {
        throw "Audio checker rejected the valid fixture (exit $validExit): $validOutput"
    }

    $protectedPaths = @($audioResultPath, $audioPointerPath, $audioManifestPath)
    $protectedContents = @($protectedPaths | ForEach-Object {
        [Convert]::ToBase64String([IO.File]::ReadAllBytes($_))
    })
    foreach ($failureCase in @('corrupt-product', 'missing-product')) {
        if ($failureCase -eq 'corrupt-product') {
            $damagedBytes = [IO.File]::ReadAllBytes($audioProductPath)
            if ($damagedBytes.Length -eq 0) { throw 'Audio fixture product must be non-empty.' }
            $damagedBytes[0] = $damagedBytes[0] -bxor 0xff
            [IO.File]::WriteAllBytes($audioProductPath, $damagedBytes)
        } else {
            Remove-Item -LiteralPath $audioProductPath -Force
        }
        $productExistsBefore = Test-Path -LiteralPath $audioProductPath -PathType Leaf
        $productBytesBefore = if ($productExistsBefore) {
            [Convert]::ToBase64String([IO.File]::ReadAllBytes($audioProductPath))
        } else { $null }

        $failureOutput = (& $childPowerShell -NoProfile -File $checkerPath `
            -GenerationExecutable $generationExecutableFull -BuildRoot $fixtureRoot `
            -SourceRoot $sourceRoot 2>&1 | ForEach-Object { $_.ToString() } | Out-String).Trim()
        $failureExit = $LASTEXITCODE
        try { $failureDiagnostic = $failureOutput | ConvertFrom-Json -ErrorAction Stop }
        catch { throw "Audio checker returned non-JSON output for $failureCase (exit $failureExit): $failureOutput" }
        if ($failureExit -eq 0 -or $failureDiagnostic.error_code -cne $failureCase -or
            $failureDiagnostic.scope -cne 'test-audio' -or
            $failureDiagnostic.verify -isnot [bool] -or !$failureDiagnostic.verify) {
            throw "Audio checker did not preserve the expected $failureCase failure (exit $failureExit): $failureOutput"
        }
        for ($i = 0; $i -lt $protectedPaths.Count; $i++) {
            $currentContents = [Convert]::ToBase64String([IO.File]::ReadAllBytes($protectedPaths[$i]))
            if ($currentContents -cne $protectedContents[$i]) {
                throw "Audio checker changed protected fixture state for ${failureCase}: $($protectedPaths[$i])"
            }
        }
        $productExistsAfter = Test-Path -LiteralPath $audioProductPath -PathType Leaf
        $productBytesAfter = if ($productExistsAfter) {
            [Convert]::ToBase64String([IO.File]::ReadAllBytes($audioProductPath))
        } else { $null }
        if ($productExistsAfter -ne $productExistsBefore -or
            $productBytesAfter -cne $productBytesBefore) {
            throw "Audio checker changed product state for $failureCase."
        }
        Write-Host "Audio checker case ${failureCase}: exit $failureExit, error_code $($failureDiagnostic.error_code), fixture state unchanged."
    }
    Write-Host 'Generation upstream identity: selected edges verified, stale mixed upstream rejected, pointer unchanged.'
    Write-Host "Audio checker: valid exit $validExit; corrupt and missing failures preserved fixture state."
}
finally {
    $parent = [IO.Path]::GetFullPath($FixtureParent).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    if (!$resolved.StartsWith($parent + [IO.Path]::DirectorySeparatorChar,
                              [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Fixture cleanup path escapes its parent.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
