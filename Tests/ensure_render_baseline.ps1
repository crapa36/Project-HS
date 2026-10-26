param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$GenerationPointer,
    [Parameter(Mandatory = $true)][string]$ArtifactsRoot,
    [string]$BaselineDirectory
)

$ErrorActionPreference = 'Stop'
$executablePath = [IO.Path]::GetFullPath($Executable)
$pointerPath = [IO.Path]::GetFullPath($GenerationPointer)
$artifactsPath = [IO.Path]::GetFullPath($ArtifactsRoot)
$captureFlags = @('--smoke', '--smoke-render-lockstep', '--validation', '--capture-diagnostics', '--ticks=180',
    '--barriers=legacy', '--resize-test')
$requiredFiles = @('result.json', 'spec.json', 'timeline.csv', 'gpu_passes.csv',
    'dynamic_instance_upload.csv', 'events.ndjson', 'render.json',
    'dred_config.json', 'particle_stats.json', 'adapter.txt',
    'renderer_features.jsonl', 'capture_tick_180.png')

function File-Sha256([string]$Path) {
    $hash = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $stream.Dispose()
        $hash.Dispose()
    }
}

function File-Identity([string]$Path, [string]$Name) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Render baseline input missing: $Path"
    }
    $file = Get-Item -LiteralPath $Path
    return [ordered]@{
        name = $Name
        size = [long]$file.Length
        sha256 = File-Sha256 $Path
    }
}

function Capture-Identity {
    $pointer = Get-Content -LiteralPath $pointerPath -Raw | ConvertFrom-Json
    if ($pointer.schema_version -ne 1 -or $pointer.scope -cne 'runtime-full' -or
        [string]$pointer.generation_id -cnotmatch '^[0-9a-f]{64}$' -or
        [string]$pointer.manifest_locator -cne
            "Generations/$($pointer.generation_id)/generation.json") {
        throw 'Render baseline requires a valid runtime-full Generation pointer.'
    }
    $buildRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path $pointerPath -Parent) '..'))
    $manifestPath = Join-Path $buildRoot ([string]$pointer.manifest_locator)
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema_version -notin @(2, 3) -or
        [string]$manifest.scope -cne 'runtime-full' -or
        [string]$manifest.generation_id -cne [string]$pointer.generation_id) {
        throw 'Render baseline Generation manifest does not match its pointer.'
    }
    # The manifest identity alone cannot detect a damaged or replaced Product
    # at the same locator. A cached capture must never skip that integrity gate.
    $generationVerifier = Join-Path $buildRoot 'hs_generation.exe'
    if (!(Test-Path -LiteralPath $generationVerifier -PathType Leaf)) {
        throw "Render baseline Generation verifier is missing: $generationVerifier"
    }
    & $generationVerifier --verify --build-root $buildRoot --scope runtime-full | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'Render baseline selected Generation failed product integrity verification.'
    }
    $inputs = New-Object System.Collections.Generic.List[object]
    $inputs.Add((File-Identity $executablePath 'ProjectHS.exe'))
    $inputs.Add((File-Identity $pointerPath 'GenerationState/runtime-full.json'))
    $inputs.Add((File-Identity $manifestPath ([string]$pointer.manifest_locator)))
    $inputs.Add((File-Identity $PSCommandPath 'Tests/ensure_render_baseline.ps1'))
    $executableRoot = Split-Path $executablePath -Parent
    foreach ($relative in @('D3D12', 'Fonts')) {
        $directory = Join-Path $executableRoot $relative
        if (!(Test-Path -LiteralPath $directory -PathType Container)) {
            throw "Render baseline runtime asset directory missing: $directory"
        }
        foreach ($file in @(Get-ChildItem -LiteralPath $directory -File -Recurse |
                Sort-Object FullName)) {
            $name = $file.FullName.Substring($executableRoot.Length + 1).Replace('\', '/')
            $inputs.Add((File-Identity $file.FullName $name))
        }
    }
    $inputs.Add((File-Identity (Join-Path $executableRoot 'WinPixEventRuntime.dll') 'WinPixEventRuntime.dll'))
    # A driver update can change pixels without changing any repository input.
    # Include every display adapter so a change to the selected adapter or its
    # driver invalidates the reusable capture.
    $adapters = @(Get-CimInstance Win32_VideoController -ErrorAction Stop |
        Sort-Object PNPDeviceID, Name |
        ForEach-Object {
            [ordered]@{
                name = [string]$_.Name
                pnp_device_id = [string]$_.PNPDeviceID
                driver_version = [string]$_.DriverVersion
            }
        })
    if ($adapters.Count -eq 0) {
        throw 'Render baseline adapter identity is unavailable.'
    }
    $document = [ordered]@{
        schema_version = 1
        executable = $executablePath
        generation_id = [string]$pointer.generation_id
        flags = $captureFlags
        adapters = $adapters
        inputs = $inputs.ToArray()
    }
    $json = $document | ConvertTo-Json -Depth 8 -Compress
    $hash = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($json)
        $identity = [BitConverter]::ToString($hash.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant()
    }
    finally { $hash.Dispose() }
    return $identity
}

function Test-Capture([string]$Directory, [string]$Identity) {
    $markerPath = Join-Path $Directory 'baseline.json'
    if (!(Test-Path -LiteralPath $markerPath -PathType Leaf)) { return $false }
    try {
        $marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
        if ($marker.schema_version -ne 1 -or
            [string]$marker.identity -cne $Identity -or
            @($marker.files).Count -lt $requiredFiles.Count) { return $false }
        $found = @{}
        foreach ($entry in $marker.files) {
            $name = [string]$entry.name
            if ($name -notmatch '^[A-Za-z0-9_.-]+$' -or $found.ContainsKey($name)) {
                return $false
            }
            $found[$name] = $true
            $path = Join-Path $Directory $name
            if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
            $file = Get-Item -LiteralPath $path
            if ($file.Length -ne [long]$entry.size -or
                (File-Sha256 $path) -cne
                    [string]$entry.sha256) { return $false }
        }
        foreach ($name in $requiredFiles) {
            if (!$found.ContainsKey($name)) { return $false }
        }
        $result = Get-Content -LiteralPath (Join-Path $Directory 'result.json') -Raw |
            ConvertFrom-Json
        $render = Get-Content -LiteralPath (Join-Path $Directory 'render.json') -Raw |
            ConvertFrom-Json
        $dred = Get-Content -LiteralPath (Join-Path $Directory 'dred_config.json') -Raw |
            ConvertFrom-Json
        if (!$result.execution_valid -or !$result.assertions_passed -or
            [int]$result.tick -ne 180 -or [int]$result.rendered_frames -ne 180 -or
            $render.enhanced_barriers -or [int]$render.validation_errors -ne 0 -or
            !$dred.enabled -or !$dred.auto_breadcrumbs -or !$dred.page_fault) {
            return $false
        }
        $timeline = @(Import-Csv -LiteralPath (Join-Path $Directory 'timeline.csv'))
        if ($timeline.Count -ne 180) { return $false }
        for ($index = 0; $index -lt 180; ++$index) {
            $tick = $index + 1
            if ([int]$timeline[$index].tick -ne $tick -or
                [int]$timeline[$index].frame -ne $tick) { return $false }
        }
        return $true
    }
    catch { return $false }
}

function Assert-ChildPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = $artifactsPath.TrimEnd([IO.Path]::DirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    if (!$full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Render baseline path escapes artifact root: $full"
    }
}

if (!(Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "ProjectHS executable does not exist: $executablePath"
}
[IO.Directory]::CreateDirectory($artifactsPath) | Out-Null
$identity = Capture-Identity
$target = Join-Path $artifactsPath $identity
Assert-ChildPath $target
if ($BaselineDirectory) {
    $given = [IO.Path]::GetFullPath($BaselineDirectory)
    if (![StringComparer]::OrdinalIgnoreCase.Equals($given, $target) -or
        !(Test-Capture $given $identity)) {
        throw "Render baseline is missing, stale, or incomplete: $given"
    }
    Write-Output $given
    return
}

$lockPath = Join-Path $artifactsPath '.capture.lock'
$lock = $null
$deadline = [DateTime]::UtcNow.AddSeconds(90)
while (!$lock) {
    try {
        $lock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate,
            [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    }
    catch [IO.IOException] {
        if ([DateTime]::UtcNow -ge $deadline) {
            throw 'Timed out waiting for the render baseline capture lock.'
        }
        Start-Sleep -Milliseconds 100
    }
}
try {
    if (Test-Capture $target $identity) {
        Write-Output $target
        return
    }
    $stage = Join-Path $artifactsPath ('.stage-' + [Guid]::NewGuid().ToString('N'))
    Assert-ChildPath $stage
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    try {
        $generationArgument = "--generation=$pointerPath"
        & $executablePath @captureFlags $generationArgument "--artifacts=$stage"
        if ($LASTEXITCODE -ne 0) { throw "Legacy render baseline capture failed: $LASTEXITCODE" }
        if ((Capture-Identity) -cne $identity) {
            throw 'Render baseline inputs changed during capture.'
        }
        $entries = @(
            foreach ($file in @(Get-ChildItem -LiteralPath $stage -File | Sort-Object Name)) {
                File-Identity $file.FullName $file.Name
            }
        )
        $marker = [ordered]@{
            schema_version = 1
            identity = $identity
            files = $entries
        }
        [IO.File]::WriteAllText((Join-Path $stage 'baseline.json'),
            (($marker | ConvertTo-Json -Depth 6) + "`n"),
            [Text.UTF8Encoding]::new($false))
        if (!(Test-Capture $stage $identity)) {
            throw 'Legacy render baseline artifacts are incomplete.'
        }
        if (Test-Path -LiteralPath $target) {
            $quarantine = Join-Path $artifactsPath
                ('.invalid-' + $identity + '-' + [Guid]::NewGuid().ToString('N'))
            Assert-ChildPath $quarantine
            [IO.Directory]::Move($target, $quarantine)
        }
        [IO.Directory]::Move($stage, $target)
    }
    finally {
        if (Test-Path -LiteralPath $stage -PathType Container) {
            $failed = Join-Path $artifactsPath ('.failed-' + [Guid]::NewGuid().ToString('N'))
            Assert-ChildPath $failed
            [IO.Directory]::Move($stage, $failed)
            Write-Warning "Failed render baseline artifacts preserved: $failed"
        }
    }
    Write-Output $target
}
finally { $lock.Dispose() }
