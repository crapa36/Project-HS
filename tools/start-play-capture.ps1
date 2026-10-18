# Record visible play until the user or computer-use controller closes this game normally.
[CmdletBinding()]
param([string]$BuildDirectory='Build/msvc-debug',[string]$OutputDirectory,
    [ValidateSet('HumanDebug','ComputerUsePerformance')][string]$Mode='HumanDebug',[string]$PythonExecutable,[switch]$SelfCheck)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repoRoot=Split-Path $PSScriptRoot -Parent
function Native-Argument([string]$Value) {
    if($Value.Contains('"') -or $Value.EndsWith('\')){throw 'Native arguments cannot contain quotes or end in a backslash.'}
    '"'+$Value+'"'
}
function Capture-Destination([string]$Value) {
    if([string]::IsNullOrWhiteSpace($Value)){$Value=Join-Path $repoRoot ('Build/play-captures/'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))}
    if(![IO.Path]::IsPathRooted($Value)){$Value=Join-Path $repoRoot $Value}
    $Value=[IO.Path]::GetFullPath($Value)
    if(Test-Path -LiteralPath $Value){throw 'Capture directory exists; recordings are never overwritten.'}
    $Value
}
function Capture-BuildPolicy([string]$CaptureMode,[string]$CacheText) {
    if($CacheText-notmatch'(?m)^CMAKE_BUILD_TYPE:[^=]+=(Debug|Release)\r?$'){throw 'Capture requires an explicit Debug or Release build.'}
    $buildType=$matches[1]
    if($CacheText-notmatch'(?m)^HS_ENABLE_VALIDATION:[^=]+=(ON|OFF)\r?$'){throw 'Capture requires an explicit D3D12 validation setting.'}
    $validation=$matches[1]-eq'ON'
    if($CacheText-notmatch'(?m)^HS_ENABLE_GPU_VALIDATION:[^=]+=(ON|OFF)\r?$'){throw 'Capture requires an explicit GPU validation setting.'}
    $gpuValidation=$matches[1]-eq'ON'
    switch($CaptureMode){
        'HumanDebug' {
            if($buildType-ne'Debug'-or!$validation){throw 'HumanDebug requires Debug with D3D12 validation enabled.'}
        }
        'ComputerUsePerformance' {
            if($buildType-ne'Release'-or$validation-or$gpuValidation){throw 'ComputerUsePerformance requires Release with D3D12 and GPU validation disabled.'}
        }
        default {throw 'Unknown capture mode.'}
    }
    [pscustomobject]@{build_type=$buildType;validation_enabled=$validation;gpu_validation_enabled=$gpuValidation}
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
if($SelfCheck){
    if((Native-Argument 'C:\example folder\result.csv')-ne'"C:\example folder\result.csv"'){throw 'Whitespace quoting failed.'}
    $rejected=$false;try{$null=Native-Argument 'invalid"argument'}catch{$rejected=$true};if(!$rejected){throw 'Invalid quote accepted.'}
    $rejected=$false;try{$null=Capture-Destination $repoRoot}catch{$rejected=$true};if(!$rejected){throw 'Existing destination accepted.'}
    $fixture=Capture-Destination ('Build/play-captures/selfcheck-'+[Guid]::NewGuid().ToString('N'))
    if(!(($fixture+[IO.Path]::DirectorySeparatorChar).StartsWith($repoRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase))){throw 'Relative destination escaped repository.'}
    $debugCache="CMAKE_BUILD_TYPE:STRING=Debug`nHS_ENABLE_VALIDATION:BOOL=ON`nHS_ENABLE_GPU_VALIDATION:BOOL=OFF`n"
    $releaseCache="CMAKE_BUILD_TYPE:STRING=Release`nHS_ENABLE_VALIDATION:BOOL=OFF`nHS_ENABLE_GPU_VALIDATION:BOOL=OFF`n"
    $debugPolicy=Capture-BuildPolicy 'HumanDebug' $debugCache
    $releasePolicy=Capture-BuildPolicy 'ComputerUsePerformance' $releaseCache
    if($debugPolicy.build_type-ne'Debug'-or!$debugPolicy.validation_enabled-or$releasePolicy.build_type-ne'Release'-or$releasePolicy.validation_enabled-or$releasePolicy.gpu_validation_enabled){throw 'Capture build policy receipt differs from cache.'}
    foreach($case in @(
        @('HumanDebug',$releaseCache),@('HumanDebug',$debugCache.Replace('HS_ENABLE_VALIDATION:BOOL=ON','HS_ENABLE_VALIDATION:BOOL=OFF')),
        @('ComputerUsePerformance',$debugCache),@('ComputerUsePerformance',$releaseCache.Replace('HS_ENABLE_VALIDATION:BOOL=OFF','HS_ENABLE_VALIDATION:BOOL=ON')),
        @('ComputerUsePerformance',$releaseCache.Replace('HS_ENABLE_GPU_VALIDATION:BOOL=OFF','HS_ENABLE_GPU_VALIDATION:BOOL=ON')),
        @('ComputerUsePerformance',"CMAKE_BUILD_TYPE:STRING=Release`n"),@('Unknown',$releaseCache))){
        $rejected=$false;try{$null=Capture-BuildPolicy $case[0] $case[1]}catch{$rejected=$true}
        if(!$rejected){throw ('Invalid capture build policy accepted: '+$case[0])}
    }
    $verifiedPython=Capture-Python '' $PythonExecutable
    if(!(Test-Path -LiteralPath $verifiedPython -PathType Leaf)){throw 'Working Python receipt is unavailable.'}
    foreach($invalidPython in @((Join-Path $repoRoot 'Build/missing-python.exe'),'C:\Users\fixture\AppData\Local\Microsoft\WindowsApps\python.exe',(Get-Command whoami.exe -ErrorAction Stop).Source)){
        $rejected=$false;try{$null=Capture-Python '' $invalidPython}catch{$rejected=$true}
        if(!$rejected){throw ('Invalid Python interpreter accepted: '+$invalidPython)}
    }
    $cachePython="_Python3_EXECUTABLE:INTERNAL=$verifiedPython`n"
    if((Capture-Python $cachePython '')-ne$verifiedPython){throw 'Configured Python was not selected.'}
    if((Capture-Python "_Python3_EXECUTABLE:INTERNAL=missing-python.exe`n" $verifiedPython)-ne$verifiedPython){throw 'Explicit working Python did not override invalid cache.'}
    'play capture launcher selfcheck passed: quoting, unique destination, repository paths, mode build/validation guards, working Python/alias/missing interpreter boundaries';exit 0
}
if(![IO.Path]::IsPathRooted($BuildDirectory)){$BuildDirectory=Join-Path $repoRoot $BuildDirectory}
$buildRoot=(Resolve-Path -LiteralPath $BuildDirectory).Path
$binary=Join-Path $buildRoot 'ProjectHS.exe';$pointer=Join-Path $buildRoot 'GenerationState/runtime-full.json';$cache=Join-Path $buildRoot 'CMakeCache.txt'
foreach($file in @($binary,$pointer,$cache)){if(!(Test-Path -LiteralPath $file -PathType Leaf)){throw "Missing capture input: $file"}}
$cacheText=[IO.File]::ReadAllText($cache)
$buildPolicy=Capture-BuildPolicy $Mode $cacheText
$computerUse=$Mode-eq'ComputerUsePerformance'
$python=Capture-Python $cacheText $PythonExecutable
$windowsPowerShell=(Get-Command powershell.exe -ErrorAction Stop).Source
$run=Capture-Destination $OutputDirectory
New-Item -ItemType Directory -Path $run|Out-Null
$sessionPath=Join-Path $run 'capture-session.json';$settingsPath=Join-Path $env:LOCALAPPDATA 'ProjectHS/settings.json'
$gpuReadyFile=Join-Path $run 'renderer_features.jsonl'
if(Test-Path -LiteralPath $settingsPath){Copy-Item -LiteralPath $settingsPath -Destination (Join-Path $run 'settings-before.json')}
Copy-Item -LiteralPath $pointer -Destination (Join-Path $run 'generation-used.json')
$arguments=@('--capture-diagnostics',('--artifacts='+$run),('--generation='+$pointer),('--playtest-output='+(Join-Path $run 'playtest')))
$session=[ordered]@{
    schema=1;state='starting';started_utc=[DateTime]::UtcNow.ToString('o');output_directory=$run;mode=$Mode;build_type=$buildPolicy.build_type;validation_enabled=$buildPolicy.validation_enabled;gpu_validation_enabled=$buildPolicy.gpu_validation_enabled;
    temporary_invulnerable=$false;human_input=(!$computerUse);input_source=$(if($computerUse){'computer_use'}else{'human'});
    settings_policy=$(if($computerUse){'isolated test profile'}else{'normal user profile; no quality/resolution/pacing/audio overrides'});
    profile_local_app_data=$env:LOCALAPPDATA;profile_root=(Split-Path $settingsPath -Parent);python_executable=$python;
    gpu_ready_file=$gpuReadyFile;gpu_counter_start_policy='wait_for_nonempty_gpu_ready_file; CPU/thread/NVIDIA start immediately';
    executable=$binary;executable_sha256=(Get-FileHash -LiteralPath $binary).Hash;generation=(Get-Content -LiteralPath $pointer -Raw|ConvertFrom-Json).generation_id;
    arguments=$arguments;stop_policy=$(if($computerUse){'computer-use controller closes game normally'}else{'user closes game'});pid=$null;process_start_ticks=$null;sampler_pid=$null;
    game_exit_code=$null;sampler_exit_code=$null;analysis_exit_code=$null;complete=$false;error=$null
}
Publish-Session $session $sessionPath
@($(if($computerUse){'Computer-use performance capture; the controller closes the game normally to finish.'}else{'Play normally in Project HS; close the game normally to finish.'}),
  'Settings changes are recorded by revision. No quality, resolution, pacing or audio override is applied.',
  'After exit the launcher writes REPORT.md, summary.json, option-costs.json and per-pass tables.',
  'Raw CPU/GPU/VRAM/QPC/settings/events/input data stays local. Nothing is uploaded.',
  'PID GPU engine usage is separate from NVIDIA whole-device metrics, which include other applications.',
  'PDH GPU/System counters wait for renderer initialization; CPU/thread/NVIDIA sampling starts immediately.',
  ($buildPolicy.build_type+' build; D3D12 validation='+$buildPolicy.validation_enabled+'; GPU validation='+$buildPolicy.gpu_validation_enabled+'. Measurement overhead is included. Unsupported counters/timestamps remain unavailable.'))|Set-Content -LiteralPath (Join-Path $run 'README.txt') -Encoding UTF8
$game=$null;$sampler=$null
try{
    $game=Start-Process -FilePath $binary -WorkingDirectory $repoRoot -WindowStyle Normal -ArgumentList ($arguments|ForEach-Object{Native-Argument $_}) -RedirectStandardOutput (Join-Path $run 'stdout.log') -RedirectStandardError (Join-Path $run 'stderr.log') -PassThru
    $null=$game.Handle
    $session.pid=$game.Id;$session.process_start_ticks=$game.StartTime.ToUniversalTime().Ticks
    $samplerArguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Native-Argument (Join-Path $PSScriptRoot 'sample-performance-utilization.ps1')),'-ProcessId',$game.Id,'-ProcessStartTicks',$session.process_start_ticks,'-MaxSeconds','0','-OutputDirectory',(Native-Argument (Join-Path $run 'utilization')))
    $samplerArguments+=@('-GpuReadyFile',(Native-Argument $gpuReadyFile))
    if($null-ne(Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue)){$samplerArguments+='-NvidiaSmi'}
    $sampler=Start-Process -FilePath $windowsPowerShell -WindowStyle Hidden -ArgumentList $samplerArguments -WorkingDirectory $repoRoot -RedirectStandardOutput (Join-Path $run 'sampler-stdout.log') -RedirectStandardError (Join-Path $run 'sampler-stderr.log') -PassThru
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
    if(Test-Path -LiteralPath $settingsPath){Copy-Item -LiteralPath $settingsPath -Destination (Join-Path $run 'settings-after.json')}
    $analysis=Start-Process -FilePath $python -WindowStyle Hidden -WorkingDirectory $repoRoot -ArgumentList @('-B',(Native-Argument (Join-Path $PSScriptRoot 'analyze-play-capture.py')),(Native-Argument $run)) -RedirectStandardOutput (Join-Path $run 'analysis-stdout.log') -RedirectStandardError (Join-Path $run 'analysis-stderr.log') -PassThru
    $null=$analysis.Handle;$analysis.WaitForExit();$session.analysis_exit_code=$analysis.ExitCode
    $diagnostics=$null
    if(Test-Path -LiteralPath (Join-Path $run 'diagnostics.json')){$diagnostics=Get-Content -LiteralPath (Join-Path $run 'diagnostics.json') -Raw|ConvertFrom-Json}
    $sampling=$null
    if(Test-Path -LiteralPath (Join-Path $run 'utilization/sampling.json')){$sampling=Get-Content -LiteralPath (Join-Path $run 'utilization/sampling.json') -Raw|ConvertFrom-Json}
    $session.complete=$null-eq$session.error-and$session.game_exit_code-eq0-and$session.sampler_exit_code-eq0-and$session.analysis_exit_code-eq0-and$null-ne$diagnostics-and$diagnostics.complete-and$null-ne$sampling-and$sampling.complete-and$null-eq$sampling.error
    $session.state=if($session.complete){'complete'}else{'incomplete'};$session['finished_utc']=[DateTime]::UtcNow.ToString('o')
    Publish-Session $session $sessionPath
    Write-Output ('Capture '+$session.state+': '+(Join-Path $run 'REPORT.md'))
    if(!$session.complete){exit 1}
}catch{
    $session.error=$_.Exception.Message;$session.state='failed';$session['failed_utc']=[DateTime]::UtcNow.ToString('o');Publish-Session $session $sessionPath
    # A running user's game and its PID-bound sampler are never terminated here.
    throw
}
