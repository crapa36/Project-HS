[CmdletBinding()]
param(
    [ValidateSet('xess', 'streamline', 'fsr', 'nrd', 'directsr', 'brixelizer', 'directstorage', 'dxc69')][string]$Sdk = 'xess',
    [switch]$Offline,
    [switch]$PrintPath,
    [switch]$BuildNative,
    [string]$Dxc
)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$lockPath = Join-Path $repoRoot 'RenderSDKs.lock.json'
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
if ($lock.schema_version -ne 1) { throw 'Unsupported renderer SDK lock schema.' }
$entry = $lock.sdks.$Sdk
$directSrPin = $entry.distribution_kind -eq 'nuget_archive' -and
    $entry.package_id -ceq 'Microsoft.Direct3D.D3D12' -and $entry.release -cmatch '^1\.715\.1-preview$' -and
    $entry.archive_uri -ceq 'https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/1.715.1-preview/microsoft.direct3d.d3d12.1.715.1-preview.nupkg'
$directStoragePin = $entry.distribution_kind -eq 'nuget_archive' -and
    $entry.package_id -ceq 'Microsoft.Direct3D.DirectStorage' -and $entry.release -ceq '1.3.0' -and
    $entry.archive_uri -ceq 'https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.directstorage/1.3.0/microsoft.direct3d.directstorage.1.3.0.nupkg' -and
    $entry.archive_sha256 -ceq '8ab6c1082537565388b79050d94fdd7f5f437cd0f1d909e7b19f786042b97177'
$nugetPin = $directSrPin -or $directStoragePin
$dxc69Pin = $Sdk -ceq 'dxc69' -and $entry.distribution_kind -ceq 'release_archive' -and
    $entry.release -ceq '1.9.2602.24' -and
    $entry.archive_uri -ceq 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2602.24/dxc_2026_05_27.zip' -and
    $entry.archive_sha256 -ceq 'cf658aacf070d3045e31b8f1f8a696c2945f37c1095019481ef7c513368db3b4'
if (!$entry -or (!$nugetPin -and !$dxc69Pin -and $entry.commit -notmatch '^[0-9a-f]{40}$') -or
    ($entry.distribution_kind -ne 'git_components' -and $entry.archive_sha256 -notmatch '^[0-9a-f]{64}$')) {
    throw "Renderer SDK $Sdk is not pinned by an exact source commit or NuGet package version and SHA-256."
}
$sdkRoot = Join-Path $repoRoot 'Build/tooling/render-sdks'
$sdkRootPrefix = [IO.Path]::GetFullPath($sdkRoot) + [IO.Path]::DirectorySeparatorChar
$repoRootPrefix = $repoRoot + [IO.Path]::DirectorySeparatorChar

function Assert-SdkPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [IO.Path]::GetFullPath($Path)
    if (!$full.StartsWith($sdkRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing a renderer SDK path outside $sdkRoot`: $full"
    }
    $ancestor = $full
    while ($ancestor.StartsWith($repoRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        $item = Get-Item -LiteralPath $ancestor -Force -ErrorAction SilentlyContinue
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing a renderer SDK path through a reparse point: $ancestor"
        }
        $ancestor = Split-Path $ancestor -Parent
    }
    return $full
}

function Get-StreamSha256 {
    param([Parameter(Mandatory = $true)][IO.Stream]$Stream)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hasher.ComputeHash($Stream))).Replace('-', '').ToLowerInvariant() }
    finally { $hasher.Dispose() }
}

$versionRoot = Assert-SdkPath (Join-Path $sdkRoot "$Sdk-$($entry.release)")
$archiveFilename = "$Sdk-$($entry.release).zip"
if ($entry.archive_filename) {
    if ($entry.archive_filename -match '[/\\:]|\.\.') { throw 'Unsafe SDK archive filename.' }
    $archiveFilename = [string]$entry.archive_filename
}
$archivePath = Assert-SdkPath (Join-Path $sdkRoot $archiveFilename)
$required = @($entry.headers) + @($entry.libraries) + @($entry.runtime) +
    @($entry.notices) + @($entry.documents)
if ($dxc69Pin) {
    $required += @($entry.tools)
    $expectedDxc69 = [ordered]@{
        'bin/x64/dxc.exe' = '1367fd29d0ebba5bf10d1041a9deff85396d30090b3651d872ec65d11a476ea4'
        'bin/x64/dxcompiler.dll' = '9b5e10ed756c461b4ec2c83a99f1d6ace20e97826e9c0b0e966b7b1cd6f2aec6'
        'bin/x64/dxil.dll' = 'cbcfe883a09fd0ca1f98abdf3a9553b560895e3283a136da82a8381253a169df'
        'bin/x64/dxv.exe' = 'f26242efb0197ffefa51eb5cb14603207d1d81fa547394020a5767bf65979f61'
        'LICENSE-LLVM.txt' = '729615317e28dd03907e46f0fc3b5e88f7853cee61d1a1471d2749335516b46f'
        'LICENSE-MIT.txt' = '903df5512f7d02609fed0c780a9b704f5a3eeb6e4d84ebe42a29845c81899a3c'
        'LICENSE-MS.txt' = '734f72f239fe7b07b4c7203f294c1a7ce27095687278bab7e56d630d7c672963'
        'inc/hlsl/LICENSE.txt' = 'f47b94fc51b04f4354a6c6f41fb84b0bd22a8ebfcde9f93c1b4d9abd7f6d14c2'
    }
    if (@($required).Count -ne $expectedDxc69.Count -or
        @($required | Select-Object -Unique).Count -ne $expectedDxc69.Count -or
        @($entry.components).Count -ne $expectedDxc69.Count) { throw 'Incomplete retail DXC 6.9 tool/notice pin.' }
    foreach ($component in $entry.components) {
        if (!$expectedDxc69.Contains([string]$component.path) -or
            $component.sha256 -cne $expectedDxc69[[string]$component.path] -or
            [string]$component.path -cnotin $required) { throw 'Retail DXC 6.9 component differs from approved pin.' }
    }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Invoke-GitComponentsBootstrap {
    # SHA256 values are checked together with the exact commit's Git blob IDs.
    # No mutable installed receipt is used as the digest authority.
    $origin = [string]$entry.component_origin
    if ($origin -cne ("https://raw.githubusercontent.com/" +
        ([uri]$entry.repository).AbsolutePath.Trim('/') + "/$($entry.commit)/")) {
        throw 'Git component origin does not identify the pinned repository commit.'
    }
    $components = @($entry.components)
    if (!$components.Count -or @($components.path | Select-Object -Unique).Count -ne $components.Count -or
        @($required | Where-Object { $_ -cnotin $components.path }).Count) {
        throw 'Pinned Git components are incomplete or duplicated.'
    }
    $partialRoot = Assert-SdkPath ($versionRoot + '.partial-' + [guid]::NewGuid().ToString('N'))
    $publish = @()
    New-Item -ItemType Directory -Force -Path $sdkRoot | Out-Null
    try {
        foreach ($component in $components) {
            $relative = [string]$component.path
            if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[/\\])\.\.([/\\]|$)' -or
                $relative.Contains(':') -or $component.sha256 -notmatch '^[0-9a-f]{64}$' -or
                $component.blob_sha1 -notmatch '^[0-9a-f]{40}$') {
                throw "Unsafe or unpinned Git component: $relative"
            }
            $installed = Assert-SdkPath (Join-Path $versionRoot $relative)
            if (Test-Path -LiteralPath $installed -PathType Leaf) { $file = $installed }
            else {
                if ($Offline) { throw "Pinned $Sdk component absent in offline mode: $relative" }
                $file = Assert-SdkPath (Join-Path $partialRoot $relative)
                New-Item -ItemType Directory -Force -Path (Split-Path $file -Parent) | Out-Null
                Invoke-WebRequest -Uri ($origin + $relative) -OutFile $file -UseBasicParsing
                $publish += [pscustomobject]@{ source = $file; target = $installed }
            }
            if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $component.sha256) {
                throw "Pinned Git component SHA256 mismatch; preserve and inspect: $relative"
            }
            $bytes = [IO.File]::ReadAllBytes($file)
            $header = [Text.Encoding]::ASCII.GetBytes(('blob ' + $bytes.Length + [char]0))
            $hasher = [Security.Cryptography.SHA1]::Create()
            try {
                $null = $hasher.TransformBlock($header, 0, $header.Length, $header, 0)
                $null = $hasher.TransformFinalBlock($bytes, 0, $bytes.Length)
                $blob = ([BitConverter]::ToString($hasher.Hash)).Replace('-', '').ToLowerInvariant()
            }
            finally { $hasher.Dispose() }
            if ($blob -cne $component.blob_sha1) { throw "Pinned Git blob mismatch: $relative" }
        }
        # Publish only after all components pass; existing altered files fail above.
        foreach ($file in $publish) {
            New-Item -ItemType Directory -Force -Path (Split-Path $file.target -Parent) | Out-Null
            Move-Item -LiteralPath (Assert-SdkPath $file.source) -Destination (Assert-SdkPath $file.target)
        }
        $receipt = [ordered]@{
            schema_version = 1; sdk = $Sdk; release = $entry.release; commit = $entry.commit;
            distribution_kind = 'git_components'; component_origin = $origin;
            verified_utc = [DateTime]::UtcNow.ToString('o');
            components = @($components | Select-Object path, sha256, blob_sha1)
        }
        $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (
            Assert-SdkPath (Join-Path $versionRoot 'bootstrap-receipt.json')) -Encoding UTF8
        return $versionRoot
    }
    finally {
        if (Test-Path -LiteralPath $partialRoot) {
            Remove-Item -LiteralPath (Assert-SdkPath $partialRoot) -Recurse -Force
        }
    }
}

function Invoke-SdkBootstrap {
    if ($entry.distribution_kind -eq 'git_components') { return Invoke-GitComponentsBootstrap }
    New-Item -ItemType Directory -Force -Path $sdkRoot | Out-Null
    Assert-SdkPath $archivePath | Out-Null
    if (!(Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        if ($Offline) { throw "Pinned $Sdk archive is absent in offline mode: $archivePath" }
        $partialArchive = Assert-SdkPath ($archivePath + '.partial-' + [guid]::NewGuid().ToString('N'))
        try {
            Invoke-WebRequest -Uri $entry.archive_uri -OutFile $partialArchive -UseBasicParsing
            $digest = (Get-FileHash -LiteralPath $partialArchive -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($digest -ne $entry.archive_sha256) {
                throw "Renderer SDK archive SHA-256 mismatch: expected $($entry.archive_sha256), got $digest."
            }
            Move-Item -LiteralPath (Assert-SdkPath $partialArchive) -Destination $archivePath
        }
        finally {
            if (Test-Path -LiteralPath $partialArchive) {
                Remove-Item -LiteralPath (Assert-SdkPath $partialArchive) -Force
            }
        }
    }
    $archiveDigest = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($archiveDigest -ne $entry.archive_sha256) {
        throw "Cached renderer SDK archive SHA-256 mismatch; preserve and inspect $archivePath."
    }

    $archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
    $partialRoot = Assert-SdkPath ($versionRoot + '.partial-' + [guid]::NewGuid().ToString('N'))
    try {
        # The release may wrap files in a top directory. Locate exactly one SDK
        # prefix, then require every locked component from that same prefix.
        $anchor = if ($dxc69Pin) { 'bin/x64/dxc.exe' } else { [string]$entry.headers[0] }
        $anchors = @($archive.Entries | Where-Object {
            $name = $_.FullName.Replace('\', '/')
            $name -eq $anchor -or $name.EndsWith('/' + $anchor, [StringComparison]::Ordinal)
        })
        if ($anchors.Count -ne 1) { throw "Pinned $Sdk archive does not contain one unambiguous SDK root." }
        $anchorName = $anchors[0].FullName.Replace('\', '/')
        $prefix = $anchorName.Substring(0, $anchorName.Length - $anchor.Length)
        $components = @()
        foreach ($relative in $required) {
            if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[/\\])\.\.([/\\]|$)' -or
                $relative.Contains(':')) { throw "Unsafe SDK lock component: $relative" }
            $matches = @($archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -ceq ($prefix + $relative) })
            if ($matches.Count -ne 1 -or $matches[0].Length -le 0) { throw "Missing or duplicate SDK component: $relative" }
            $stream = $matches[0].Open()
            try { $digest = Get-StreamSha256 $stream } finally { $stream.Dispose() }
            if ($dxc69Pin -and $digest -cne $expectedDxc69[[string]$relative]) {
                throw "Retail DXC 6.9 archive component SHA-256 mismatch: $relative"
            }
            $components += [pscustomobject]@{ path = [string]$relative; sha256 = $digest; zip_entry = $matches[0] }
        }

        if (Test-Path -LiteralPath $versionRoot) {
            # Compare installed bytes against entries of the verified archive,
            # rather than trusting a mutable local receipt as the digest authority.
            $missingComponents = @()
            foreach ($component in $components) {
                $installed = Assert-SdkPath (Join-Path $versionRoot $component.path)
                if (!(Test-Path -LiteralPath $installed -PathType Leaf)) {
                    $missingComponents += $component
                }
                elseif ((Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash.ToLowerInvariant() -ne $component.sha256) {
                    throw "Existing renderer SDK component differs from its pinned archive: $installed"
                }
            }
            # Extend a verified partial installation from the same immutable
            # archive. Validate every staged byte before publishing new files.
            if ($missingComponents.Count) {
                New-Item -ItemType Directory -Path $partialRoot | Out-Null
                foreach ($component in $missingComponents) {
                    $target = Assert-SdkPath (Join-Path $partialRoot $component.path)
                    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
                    $inputStream = $component.zip_entry.Open()
                    $outputStream = $null
                    try {
                        $outputStream = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
                        $inputStream.CopyTo($outputStream)
                    }
                    finally {
                        if ($outputStream) { $outputStream.Dispose() }
                        $inputStream.Dispose()
                    }
                    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $component.sha256) {
                        throw "Extracted renderer SDK component SHA-256 mismatch: $target"
                    }
                }
                foreach ($component in $missingComponents) {
                    $source = Assert-SdkPath (Join-Path $partialRoot $component.path)
                    $target = Assert-SdkPath (Join-Path $versionRoot $component.path)
                    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
                    Move-Item -LiteralPath $source -Destination $target
                }
            }
        }
        else {
            New-Item -ItemType Directory -Path $partialRoot | Out-Null
            foreach ($component in $components) {
                $target = Assert-SdkPath (Join-Path $partialRoot $component.path)
                New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
                $inputStream = $component.zip_entry.Open()
                $outputStream = $null
                try {
                    $outputStream = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
                    $inputStream.CopyTo($outputStream)
                }
                finally {
                    if ($outputStream) { $outputStream.Dispose() }
                    $inputStream.Dispose()
                }
                if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $component.sha256) {
                    throw "Extracted renderer SDK component SHA-256 mismatch: $target"
                }
            }
            Move-Item -LiteralPath (Assert-SdkPath $partialRoot) -Destination $versionRoot
        }
        $receipt = [ordered]@{
            schema_version = 1; sdk = $Sdk; release = $entry.release; commit = $entry.commit;
            archive_uri = $entry.archive_uri; archive_sha256 = $archiveDigest;
            verified_utc = [DateTime]::UtcNow.ToString('o');
            components = @($components | Select-Object path, sha256)
        }
        $receiptPath = Assert-SdkPath (Join-Path $versionRoot 'bootstrap-receipt.json')
        $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
        return $versionRoot
    }
    finally {
        $archive.Dispose()
        if (Test-Path -LiteralPath $partialRoot) {
            Remove-Item -LiteralPath (Assert-SdkPath $partialRoot) -Recurse -Force
        }
    }
}

$bootstrapMutex = New-Object System.Threading.Mutex($false, "Local\ProjectHS-RenderSDK-$Sdk-$($entry.release)")
$mutexHeld = $false
try {
    try { $mutexHeld = $bootstrapMutex.WaitOne([TimeSpan]::FromMinutes(10)) }
    catch [System.Threading.AbandonedMutexException] { $mutexHeld = $true }
    if (!$mutexHeld) { throw 'Timed out waiting for renderer SDK bootstrap.' }
    if ($Sdk -eq 'nrd') {
        . (Join-Path $PSScriptRoot 'bootstrap_nrd.ps1')
        $resolved = Invoke-NrdBootstrap
    }
    elseif ($Sdk -eq 'brixelizer') {
        if ($BuildNative -or $Dxc) { throw '-BuildNative and -Dxc apply only to the NRD source package.' }
        if ($entry.release -cne '1.1.4' -or
            $entry.commit -cne 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55' -or
            $entry.archive_sha256 -cne 'b5bba71656e18f4c813fda4eb364dcb5e9b8491efd6abb66fdc8a2b530bfcedd' -or
            $entry.source_manifest_sha256 -cne '1f5bf0cb5a2541626c66546b0a22ac410c747fb6273c65bf3f8c6d54e9bb2761') {
            throw 'Brixelizer lock differs from the pinned SDK bootstrap contract.'
        }
        $resolved = & (Join-Path $PSScriptRoot 'bootstrap-brixelizer-sdk.ps1') -Offline:$Offline -PrintPath
        if (!$resolved -or !(Test-Path -LiteralPath $resolved -PathType Container)) {
            throw 'Pinned Brixelizer SDK bootstrap returned no verified installation.'
        }
    }
    else {
        if ($BuildNative -or $Dxc) { throw '-BuildNative and -Dxc apply only to the NRD source package.' }
        $resolved = Invoke-SdkBootstrap
    }
    if ($nugetPin) {
        [xml]$metadata = Get-Content -LiteralPath (Join-Path $resolved ($entry.package_id + '.nuspec')) -Raw
        if ($metadata.package.metadata.id -cne $entry.package_id -or $metadata.package.metadata.version -cne $entry.release) {
            throw 'Pinned renderer NuGet package identity differs from its metadata.'
        }
    }
    if ($PrintPath) { Write-Output $resolved }
    elseif ($Sdk -eq 'nrd') { Write-Host "Pinned NRD source and derived native build verified at $resolved." }
    else { Write-Host "Pinned $Sdk SDK bytes verified at $resolved; SDK code has not been executed." }
}
finally {
    if ($mutexHeld) { $bootstrapMutex.ReleaseMutex() }
    $bootstrapMutex.Dispose()
}
