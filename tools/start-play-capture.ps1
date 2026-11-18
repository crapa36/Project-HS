# Record visible play until the user or computer-use controller closes this game normally.
[CmdletBinding()]
param([string]$BuildDirectory='Build/msvc-debug',[string]$OutputDirectory,[string]$PythonExecutable)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repoRoot=Split-Path $PSScriptRoot -Parent
function Native-Argument([string]$Value) {
    if($Value.Contains('"') -or $Value.EndsWith('\')){throw 'Native arguments cannot contain quotes or end in a backslash.'}
    '"'+$Value+'"'
}
function Capture-Destination([string]$Value) {
    if([string]::IsNullOrWhiteSpace($Value)){$Value=Join-Path $buildRoot ('Artifacts/play-captures/'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))}
    if(![IO.Path]::IsPathRooted($Value)){$Value=Join-Path $repoRoot $Value}
    $Value=[IO.Path]::GetFullPath($Value)
    if(Test-Path -LiteralPath $Value){throw 'Capture directory exists; recordings are never overwritten.'}
    $Value
}
function Capture-Python([string]$CacheText,[string]$ExplicitExecutable) {
    $candidates=@()
    if($ExplicitExecutable){$candidates=@($ExplicitExecutable)}else{
        if($CacheText-match'(?m)^_?Python3_EXECUTABLE:[^=]+=(.+?)\r?$'){$candidates+=$matches[1]}
        $candidates+=@(Get-Command python.exe -All -ErrorAction SilentlyContinue | ForEach-Object {$_.Source})
    }
    foreach($candidate in $candidates){
        if(!$candidate-or$candidate.Replace('/','\')-match'\\Microsoft\\WindowsApps\\python[^\\]*\.exe$'-or!(Test-Path -LiteralPath $candidate -PathType Leaf)){continue}
        try{
            $probe=@(& $candidate -c "import csv, importlib.util, json, math, pathlib, tempfile; print('hs_play_capture_python_ready')" 2>&1)
            if($LASTEXITCODE-eq0-and$probe-contains'hs_play_capture_python_ready'){return (Resolve-Path -LiteralPath $candidate).Path}
        }catch{continue}
    }
    throw 'A working Python interpreter is required; WindowsApps aliases are unavailable. Pass -PythonExecutable with the interpreter path.'
}
function Publish-Session($Data,[string]$Path) {
    [IO.File]::WriteAllText(($Path+'.tmp'),($Data|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath ($Path+'.tmp') -Destination $Path -Force
}
if(![IO.Path]::IsPathRooted($BuildDirectory)){$BuildDirectory=Join-Path $repoRoot $BuildDirectory}
$buildRoot=(Resolve-Path -LiteralPath $BuildDirectory).Path
$binary=Join-Path $buildRoot 'bin/ProjectHS.exe'
$pack=Join-Path $buildRoot 'bin/Content/content.hsp'
$cache=Join-Path $buildRoot 'CMakeCache.txt'
foreach($file in @($binary,$pack,$cache)){if(!(Test-Path -LiteralPath $file -PathType Leaf)){throw "Missing capture input: $file"}}
$cacheText=[IO.File]::ReadAllText($cache)
if ($cacheText -notmatch '(?m)^CMAKE_BUILD_TYPE:[^=]+=Debug\r?$') { throw 'Play recording requires a Debug build with capture diagnostics.' }
$python=Capture-Python $cacheText $PythonExecutable
$windowsPowerShell=(Get-Command powershell.exe -ErrorAction Stop).Source
$run=Capture-Destination $OutputDirectory
New-Item -ItemType Directory -Path $run|Out-Null
$sessionPath=Join-Path $run 'capture-session.json'
$gpuReadyFile=Join-Path $run 'diagnostics.json'
$arguments=@('--capture-diagnostics','--validation','--mute',('--artifacts='+$run),('--playtest-output='+(Join-Path $run 'playtest')))
$session=[ordered]@{
    schema=1;state='starting';started_utc=[DateTime]::UtcNow.ToString('o');output_directory=$run;build_type='Debug';validation_enabled=$true;gpu_validation_enabled=$false;
    executable=$binary;executable_sha256=(Get-FileHash -LiteralPath $binary).Hash;content_pack=$pack;
    arguments=$arguments;pid=$null;process_start_ticks=$null;sampler_pid=$null;
    game_exit_code=$null;sampler_exit_code=$null;analysis_exit_code=$null;complete=$false;error=$null
}
Publish-Session $session $sessionPath
$game=$null;$sampler=$null
try{
    $game=Start-Process -FilePath $binary -WorkingDirectory $repoRoot -WindowStyle Normal -ArgumentList ($arguments|ForEach-Object{Native-Argument $_}) -RedirectStandardError (Join-Path $run 'stderr.log') -PassThru
    $null=$game.Handle
    $session.pid=$game.Id;$session.process_start_ticks=$game.StartTime.ToUniversalTime().Ticks
    $samplerArguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Native-Argument (Join-Path $PSScriptRoot 'sample-performance-utilization.ps1')),'-ProcessId',$game.Id,'-ProcessStartTicks',$session.process_start_ticks,'-MaxSeconds','0','-OutputDirectory',(Native-Argument (Join-Path $run 'utilization')))
    $samplerArguments+=@('-GpuReadyFile',(Native-Argument $gpuReadyFile))
    if($null-ne(Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue)){$samplerArguments+='-NvidiaSmi'}
    $sampler=Start-Process -FilePath $windowsPowerShell -WindowStyle Hidden -ArgumentList $samplerArguments -WorkingDirectory $repoRoot -RedirectStandardError (Join-Path $run 'sampler-stderr.log') -PassThru
    $null=$sampler.Handle
    $session.sampler_pid=$sampler.Id;$session.state='recording';Publish-Session $session $sessionPath
    Write-Output ('Recording game PID '+$game.Id+' until exit. Capture: '+$run)
    while(!$game.WaitForExit(1000)){
        if($sampler.HasExited-and!$game.HasExited-and$null-eq$session.sampler_exit_code){
            $sampler.WaitForExit();$session.sampler_exit_code=$sampler.ExitCode
            $session.error='Utilization sampler exited before game; Runtime recording continues. Inspect sampler logs.'
            Publish-Session $session $sessionPath
        }
    }
    $game.WaitForExit();$session.game_exit_code=$game.ExitCode;$session.state='finalizing';Publish-Session $session $sessionPath
    $sampler.WaitForExit();$session.sampler_exit_code=$sampler.ExitCode
    $analysis=Start-Process -FilePath $python -WindowStyle Hidden -WorkingDirectory $repoRoot -ArgumentList @('-B',(Native-Argument (Join-Path $PSScriptRoot 'analyze-play-capture.py')),(Native-Argument $run)) -RedirectStandardError (Join-Path $run 'analysis-stderr.log') -PassThru
    $null=$analysis.Handle;$analysis.WaitForExit();$session.analysis_exit_code=$analysis.ExitCode
    $diagnostics=$null
    if(Test-Path -LiteralPath (Join-Path $run 'diagnostics.json')){$diagnostics=Get-Content -LiteralPath (Join-Path $run 'diagnostics.json') -Raw|ConvertFrom-Json}
    $sampling=$null
    if(Test-Path -LiteralPath (Join-Path $run 'utilization/sampling.json')){$sampling=Get-Content -LiteralPath (Join-Path $run 'utilization/sampling.json') -Raw|ConvertFrom-Json}
    $session.complete=$null-eq$session.error-and$session.game_exit_code-eq0-and$session.sampler_exit_code-eq0-and$session.analysis_exit_code-eq0-and$null-ne$diagnostics-and$diagnostics.complete-and$null-ne$sampling-and$sampling.complete-and!$sampling.PSObject.Properties['error']
    $session.state=if($session.complete){'complete'}else{'incomplete'};$session['finished_utc']=[DateTime]::UtcNow.ToString('o')
    Publish-Session $session $sessionPath
    Write-Output ('Capture '+$session.state+': '+(Join-Path $run 'REPORT.md'))
    if(!$session.complete){exit 1}
}catch{
    $session.error=$_.Exception.Message;$session.state='failed';$session['failed_utc']=[DateTime]::UtcNow.ToString('o');Publish-Session $session $sessionPath
    # A running user's game and its PID-bound sampler are never terminated here.
    throw
}
