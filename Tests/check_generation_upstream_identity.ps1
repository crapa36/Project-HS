param(
    [Parameter(Mandatory = $true)][string]$GenerationExecutable,
    [Parameter(Mandatory = $true)][string]$FixtureParent
)

$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path ([IO.Path]::GetFullPath($FixtureParent)) (
    'generation-upstream-' + [Guid]::NewGuid().ToString('N'))
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
    Write-Host 'Generation upstream identity: selected edges verified, stale mixed upstream rejected, pointer unchanged.'
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
