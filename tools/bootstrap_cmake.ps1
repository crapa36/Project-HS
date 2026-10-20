[CmdletBinding()]
param(
    [switch]$PrintPath
)

$ErrorActionPreference = 'Stop'

$version = '4.4.3'
$downloadUrl = 'https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-windows-x86_64.zip'
$expectedSha256 = '4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab'
$repoRoot = Split-Path $PSScriptRoot -Parent
$toolingRoot = Join-Path $repoRoot 'Build/tooling/cmake-4.4.3'
$toolingRootPrefix = [IO.Path]::GetFullPath($toolingRoot) + [IO.Path]::DirectorySeparatorChar
$repoRootPrefix = [IO.Path]::GetFullPath($repoRoot) + [IO.Path]::DirectorySeparatorChar

function Assert-ContainedPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $fullPath = [IO.Path]::GetFullPath($Path)
    if (!$fullPath.StartsWith($toolingRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to access a path outside the repo-local CMake tooling directory: $fullPath"
    }
    $ancestor = $fullPath
    while ($ancestor.StartsWith($repoRootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        $item = Get-Item -LiteralPath $ancestor -Force -ErrorAction SilentlyContinue
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing a repo-local CMake path through a reparse point: $ancestor"
        }
        $ancestor = Split-Path $ancestor -Parent
    }
    return $fullPath
}

$archiveName = 'cmake-4.4.3-windows-x86_64.zip'
$archivePath = Assert-ContainedPath (Join-Path $toolingRoot $archiveName)
$installName = 'cmake-4.4.3-windows-x86_64'
$installPath = Assert-ContainedPath (Join-Path $toolingRoot $installName)
$cmakePath = Assert-ContainedPath (Join-Path $installPath 'bin/cmake.exe')

function Test-CMakeVersion {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }

    try {
        $versionOutput = (& $Path --version 2>&1 | Out-String).Trim()
        $versionExitCode = $LASTEXITCODE
    }
    catch {
        return $false
    }

    if ($versionExitCode -ne 0) {
        return $false
    }

    $versionPattern = '(?m)^cmake version ' + [regex]::Escape($version) + '\s*$'
    return $versionOutput -match $versionPattern
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-CMakeBootstrap {
New-Item -ItemType Directory -Force -Path $toolingRoot | Out-Null

if (Test-CMakeVersion -Path $cmakePath) {
    return $cmakePath
}

if (Test-Path -LiteralPath $installPath) {
    Assert-ContainedPath $installPath | Out-Null
    Write-Host "Removing invalid repo-local CMake installation at $installPath"
    Remove-Item -LiteralPath $installPath -Recurse -Force
}

if (Test-Path -LiteralPath $archivePath -PathType Leaf) {
    $archiveHash = Get-Sha256 -Path $archivePath
    if ($archiveHash -ne $expectedSha256) {
        Write-Host "Removing CMake archive with unexpected SHA-256: $archivePath"
        Remove-Item -LiteralPath $archivePath -Force
    }
}

if (!(Test-Path -LiteralPath $archivePath -PathType Leaf)) {
    $partialArchivePath = Assert-ContainedPath (Join-Path $toolingRoot ($archiveName + '.partial-' + [guid]::NewGuid().ToString('N')))
    try {
        Write-Host "Downloading CMake $version"
        Invoke-WebRequest -Uri $downloadUrl -OutFile $partialArchivePath -UseBasicParsing
        $partialArchiveHash = Get-Sha256 -Path $partialArchivePath
        if ($partialArchiveHash -ne $expectedSha256) {
            throw "Downloaded CMake archive SHA-256 mismatch. Expected $expectedSha256, got $partialArchiveHash."
        }
        Move-Item -LiteralPath (Assert-ContainedPath $partialArchivePath) -Destination (Assert-ContainedPath $archivePath)
    }
    finally {
        if (Test-Path -LiteralPath $partialArchivePath) {
            Remove-Item -LiteralPath (Assert-ContainedPath $partialArchivePath) -Force -ErrorAction SilentlyContinue
        }
    }
}

$archiveHash = Get-Sha256 -Path $archivePath
if ($archiveHash -ne $expectedSha256) {
    throw "CMake archive SHA-256 mismatch. Expected $expectedSha256, got $archiveHash."
}

$partialInstallPath = Assert-ContainedPath (Join-Path $toolingRoot ($installName + '.partial-' + [guid]::NewGuid().ToString('N')))
try {
    Write-Host "Extracting CMake $version"
    New-Item -ItemType Directory -Force -Path $partialInstallPath | Out-Null
    Expand-Archive -LiteralPath $archivePath -DestinationPath $partialInstallPath
    $extractedInstallPath = Assert-ContainedPath (Join-Path $partialInstallPath $installName)
    $extractedCMakePath = Assert-ContainedPath (Join-Path $extractedInstallPath 'bin/cmake.exe')
    if (!(Test-CMakeVersion -Path $extractedCMakePath)) {
        throw "Extracted CMake did not report version $version."
    }
    Move-Item -LiteralPath (Assert-ContainedPath $extractedInstallPath) -Destination (Assert-ContainedPath $installPath)
}
finally {
    if (Test-Path -LiteralPath $partialInstallPath) {
        Remove-Item -LiteralPath (Assert-ContainedPath $partialInstallPath) -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if (!(Test-CMakeVersion -Path $cmakePath)) {
    throw "Repo-local CMake installation did not report version $version."
}

return $cmakePath
}

$bootstrapMutex = New-Object System.Threading.Mutex($false, 'Local\ProjectHS-CMake-4.4.3')
$mutexHeld = $false
try {
    try {
        $mutexHeld = $bootstrapMutex.WaitOne([TimeSpan]::FromMinutes(10))
    }
    catch [System.Threading.AbandonedMutexException] {
        $mutexHeld = $true
    }
    if (!$mutexHeld) {
        throw 'Timed out waiting for another repo-local CMake bootstrap to finish.'
    }

    $resolvedCMakePath = Invoke-CMakeBootstrap
    if ($PrintPath) {
        Write-Output $resolvedCMakePath
    }
    else {
        Write-Host "Repo-local CMake $version is ready at $resolvedCMakePath"
    }
}
finally {
    if ($mutexHeld) {
        $bootstrapMutex.ReleaseMutex()
    }
    $bootstrapMutex.Dispose()
}
