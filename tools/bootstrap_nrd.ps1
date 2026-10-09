# Invoked only by bootstrap_render_sdks.ps1, inside its bounded SDK mutex.
# Archive hashes identify publisher source; the build receipt identifies derived
# bytes and the actual compiler inputs, and is not a publisher DLL signature.
function Get-NrdFileDigest {
    param([string]$Path)
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "NRD input absent: $Path" }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Expand-NrdSource {
    param($Package, [string]$Archive, [string]$Target, [string[]]$Directories)
    $Archive = Assert-SdkPath $Archive
    $Target = Assert-SdkPath $Target
    if (!(Test-Path -LiteralPath $Archive)) {
        if ($Offline) { throw "Pinned NRD prerequisite absent offline: $Archive" }
        Invoke-WebRequest -Uri $Package.archive_uri -OutFile $Archive -UseBasicParsing
    }
    if ((Get-NrdFileDigest $Archive) -cne $Package.archive_sha256) {
        throw "Pinned source archive SHA-256 mismatch: $Archive"
    }
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $prefix = [string]$Package.archive_prefix
        $count = 0
        foreach ($item in $zip.Entries) {
            if (!$item.FullName.StartsWith($prefix, [StringComparison]::Ordinal)) { continue }
            $relative = $item.FullName.Substring($prefix.Length)
            if (!$relative -or $relative.EndsWith('/')) { continue }
            if ($relative -match '(^|/)\.\.(/|$)' -or $relative.Contains(':') -or $relative.Contains('\')) {
                throw "Unsafe NRD source archive entry: $relative"
            }
            if ($Directories.Count -and $relative.Contains('/') -and
                $relative.Split('/')[0] -cnotin $Directories) { continue }
            $targetPath = Assert-SdkPath (Join-Path $Target $relative)
            $sourceStream = $item.Open()
            try { $expected = Get-StreamSha256 $sourceStream }
            finally { $sourceStream.Dispose() }
            if (Test-Path -LiteralPath $targetPath) {
                if ((Get-NrdFileDigest $targetPath) -cne $expected) {
                    throw "Pinned NRD source changed: $targetPath"
                }
            }
            else {
                New-Item -ItemType Directory -Force -Path (Split-Path $targetPath -Parent) | Out-Null
                $sourceStream = $item.Open()
                $destinationStream = [IO.File]::Open($targetPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
                try { $sourceStream.CopyTo($destinationStream) }
                finally { $destinationStream.Dispose(); $sourceStream.Dispose() }
                if ((Get-NrdFileDigest $targetPath) -cne $expected) { throw 'NRD source extraction changed bytes.' }
            }
            $count++
        }
        if (!$count) { throw "Pinned source prefix absent: $prefix" }
    }
    finally { $zip.Dispose() }
}

function Copy-NrdDerivedFile {
    param([string]$Source, [string]$Relative)
    $target = Assert-SdkPath (Join-Path $versionRoot $Relative)
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $target -Force
    return [ordered]@{ path = $Relative; sha256 = Get-NrdFileDigest $target }
}

function Invoke-NrdBootstrap {
    New-Item -ItemType Directory -Force -Path $sdkRoot | Out-Null
    $sourceArchive = Assert-SdkPath (Join-Path $sdkRoot 'nrd-4.17.3-source.zip')
    $sourceRoot = Assert-SdkPath (Join-Path $versionRoot 'source')
    Expand-NrdSource $entry $sourceArchive $sourceRoot @('Include', 'Source', 'Shaders', 'Integration', 'Resources')
    $dependencyRoots = @{}
    foreach ($dependency in $entry.dependencies) {
        $dependencyArchive = Assert-SdkPath (Join-Path $sdkRoot "$($dependency.name)-$($dependency.commit).zip")
        $dependencyRoot = Assert-SdkPath (Join-Path $sdkRoot "$($dependency.name)-$($dependency.commit)")
        Expand-NrdSource $dependency $dependencyArchive $dependencyRoot @()
        $dependencyRoots[$dependency.name] = $dependencyRoot
    }
    # The flat public surface is available for renderer includes. Its bytes are
    # checked against the immutable archive extraction on every invocation.
    foreach ($relative in $entry.headers + $entry.documents) {
        $source = Join-Path $sourceRoot $relative
        $target = Assert-SdkPath (Join-Path $versionRoot $relative)
        $digest = Get-NrdFileDigest $source
        if (Test-Path -LiteralPath $target) {
            if ((Get-NrdFileDigest $target) -cne $digest) { throw "Pinned NRD public input changed: $relative" }
        }
        else { $null = Copy-NrdDerivedFile $source $relative }
    }
    $wrapper = Join-Path $repoRoot 'cmake/NRDPrerequisite/CMakeLists.txt'
    $bootstrap = Join-Path $PSScriptRoot 'bootstrap_nrd.ps1'
    $recipe = [ordered]@{
        release = $entry.release; commit = $entry.commit; archive_sha256 = $entry.archive_sha256
        dependencies = @($entry.dependencies | Select-Object name, commit, archive_sha256)
        configuration = $entry.native_configuration
        wrapper_sha256 = Get-NrdFileDigest $wrapper
        bootstrap_sha256 = Get-NrdFileDigest $bootstrap
    }
    $recipeJson = $recipe | ConvertTo-Json -Depth 8 -Compress
    $receiptPath = Assert-SdkPath (Join-Path $versionRoot 'native-build-receipt.json')
    if ($BuildNative) {
        if (!$Dxc) { throw 'NRD native build requires -Dxc <existing Windows SDK dxc.exe>.' }
        $compiler = [IO.Path]::GetFullPath($Dxc)
        $compilerDirectory = Split-Path $compiler -Parent
        $compilerPaths = @($compiler, (Join-Path $compilerDirectory 'dxcompiler.dll'), (Join-Path $compilerDirectory 'dxil.dll'))
        foreach ($path in $compilerPaths) { $null = Get-NrdFileDigest $path }
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $installation = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
        if (!$installation) { throw 'NRD prerequisite requires the installed MSVC x64 toolchain.' }
        $devCmd = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
        $environment = & $env:ComSpec /d /c "`"$devCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
        if ($LASTEXITCODE) { throw 'MSVC environment initialization failed.' }
        foreach ($line in $environment) {
            if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
        }
        # Some hosted shells inherit both Path and PATH. Do not let the second
        # inherited entry overwrite the initialized compiler search directory.
        $toolBin = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64'
        $initializedPath = @($environment | Where-Object { $_ -match '^PATH=' -and $_.IndexOf($toolBin, [StringComparison]::OrdinalIgnoreCase) -ge 0 } | Select-Object -First 1)
        if (!$initializedPath.Count) { throw 'MSVC initialized PATH absent.' }
        $env:Path = $initializedPath[0].Substring(5)
        $cl = Join-Path $toolBin 'cl.exe'
        $ninja = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
        $cmake = (& (Join-Path $PSScriptRoot 'bootstrap_cmake.ps1') -PrintPath | Select-Object -Last 1)
        $buildRoot = Assert-SdkPath (Join-Path $versionRoot 'native-build')
        $buildSource = Assert-SdkPath (Join-Path $versionRoot 'build-source')
        New-Item -ItemType Directory -Force -Path $buildSource | Out-Null
        Copy-Item -Path (Join-Path $sourceRoot '*') -Destination $buildSource -Recurse -Force
        $configure = @('-S', (Split-Path $wrapper -Parent), '-B', $buildRoot, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_C_COMPILER=$cl", "-DCMAKE_CXX_COMPILER=$cl", "-DHS_NRD_SOURCE=$buildSource",
            "-DHS_SHADERMAKE_SOURCE=$($dependencyRoots.shadermake)", "-DHS_MATHLIB_SOURCE=$($dependencyRoots.mathlib)",
            "-DHS_NRD_DXC=$compiler")
        & $cmake @configure | Out-Host
        if ($LASTEXITCODE) { throw 'Pinned NRD prerequisite configuration failed.' }
        & $cmake --build $buildRoot --target NRD --config Release --parallel 4 | Out-Host
        if ($LASTEXITCODE) { throw 'Pinned NRD prerequisite build failed.' }
        $artifacts = @()
        $artifacts += Copy-NrdDerivedFile (Join-Path $buildRoot 'bin/NRD.dll') 'bin/NRD.dll'
        $artifacts += Copy-NrdDerivedFile (Join-Path $buildSource 'Shaders/NRDConfig.hlsli') 'generated/NRDConfig.hlsli'
        $artifacts += Copy-NrdDerivedFile (Join-Path $sourceRoot 'LICENSE.txt') 'LICENSE.txt'
        $artifacts += Copy-NrdDerivedFile (Join-Path $dependencyRoots.shadermake 'LICENSE.txt') 'ShaderMake-LICENSE.txt'
        $artifacts += Copy-NrdDerivedFile (Join-Path $dependencyRoots.mathlib 'LICENSE.txt') 'MathLib-LICENSE.txt'
        $tools = @()
        $toolPaths = $compilerPaths + @($cmake, $ninja, $cl, (Join-Path $toolBin 'link.exe'),
            (Join-Path $buildRoot 'bin/ShaderMake.exe'))
        foreach ($path in $toolPaths) { $tools += [ordered]@{ path = $path; sha256 = Get-NrdFileDigest $path } }
        [ordered]@{ schema_version = 1; kind = 'derived_native_build'; recipe = $recipe; tools = $tools; artifacts = $artifacts } |
            ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
    }
    if (!(Test-Path -LiteralPath $receiptPath)) {
        throw 'Pinned NRD native build absent. Run tools/bootstrap_render_sdks.ps1 -Sdk nrd -Offline -BuildNative -Dxc <existing dxc.exe>.'
    }
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    if ($receipt.schema_version -ne 1 -or $receipt.kind -cne 'derived_native_build' -or
        ($receipt.recipe | ConvertTo-Json -Depth 8 -Compress) -cne $recipeJson) {
        throw 'NRD native recipe changed; rebuild the pinned prerequisite explicitly with -BuildNative.'
    }
    foreach ($relative in $entry.runtime + $entry.notices + $entry.generated) {
        $artifact = @($receipt.artifacts | Where-Object { $_.path -ceq $relative })
        if ($artifact.Count -ne 1 -or $artifact[0].sha256 -notmatch '^[0-9a-f]{64}$' -or
            (Get-NrdFileDigest (Assert-SdkPath (Join-Path $versionRoot $relative))) -cne $artifact[0].sha256) {
            throw "NRD derived artifact changed: $relative"
        }
    }
    foreach ($tool in $receipt.tools) {
        if ($tool.sha256 -notmatch '^[0-9a-f]{64}$' -or (Get-NrdFileDigest $tool.path) -cne $tool.sha256) {
            throw "NRD compiler input changed: $($tool.path); rebuild explicitly."
        }
    }
    return $versionRoot
}
