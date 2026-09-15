$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$work = Join-Path $repo 'Build/tooling/sccache'
$cache = Join-Path $work 'sccache-v0.17.0-x86_64-pc-windows-msvc/sccache.exe'
$vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
foreach ($line in (& $env:ComSpec /d /c "`"$dev`" -no_logo -arch=x64 -host_arch=x64 >nul && set")) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}
$env:SCCACHE_DIR = Join-Path $work 'cache'
$env:SCCACHE_SERVER_PORT = '44261'
$env:VSLANG = '1033'
$entry = (Get-Content (Join-Path $repo 'Build/msvc-core/compile_commands.json') -Raw | ConvertFrom-Json) | Where-Object file -Like '*/simulation_combat.cpp'
if (!$entry) { throw 'Compile database must contain simulation_combat.cpp.' }
$object = Join-Path $work 'probe.obj'
$command = $entry.command -replace '[-/]Zi\b','/Z7' -replace '/Fo\S+',('/Fo"' + $object + '"') -replace '/Fd\S+',''
$results = @()
Push-Location $entry.directory
try {
    & $cache --zero-stats | Out-Null
    for ($sample = 1; $sample -le 3; $sample++) {
        $probe = $command + ' /DHS_CACHE_PROBE=' + $sample
        foreach ($mode in @('direct','cold','warm')) {
            $run = if ($mode -eq 'direct') { $probe } else { '"' + $cache + '" ' + $probe }
            $timer = [Diagnostics.Stopwatch]::StartNew()
            & $env:ComSpec /d /c $run *> (Join-Path $work "$sample-$mode.log")
            $code = $LASTEXITCODE
            $timer.Stop()
            if ($code -ne 0) { throw "Compilation failed: $sample-$mode. Inspect log." }
            $results += [pscustomobject]@{ sample=$sample; mode=$mode; milliseconds=$timer.Elapsed.TotalMilliseconds; object_sha256=(Get-FileHash $object).Hash }
        }
        if ($results[-1].object_sha256 -ne $results[-2].object_sha256) { throw 'Cache hit object differs from cold output.' }
    }
    & $cache --show-stats *> (Join-Path $work 'stats.txt')
    $results | ConvertTo-Json | Set-Content (Join-Path $work 'measurements.json')
    $results | Select-Object sample,mode,milliseconds | Format-Table
    Get-Content (Join-Path $work 'stats.txt') -TotalCount 18
}
finally {
    & $cache --stop-server *> (Join-Path $work 'stop.log')
    Pop-Location
}
