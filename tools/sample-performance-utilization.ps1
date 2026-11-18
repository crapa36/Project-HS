# Process/thread CPU uses QPC wall time; GPU engine counters are PID-specific.
# MaxSeconds=0 follows the original PID/start time until exit. Whole-device metrics include other applications.
param(
    [int]$ProcessId,
    [ValidateRange(0, 2147483647)][int]$MaxSeconds = 0,
    [long]$ProcessStartTicks = 0,
    [string]$AdapterLuid,
    [string]$GpuReadyFile,
    [switch]$NoGpuCounters,
    [string]$OutputDirectory,
    [switch]$NvidiaSmi,
    [switch]$CounterWorker
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$culture = [Globalization.CultureInfo]::InvariantCulture

function Parse-GpuInstance([string]$Name) {
    $match = [regex]::Match($Name, '^pid_(\d+)_luid_(0x[0-9a-f]+_0x[0-9a-f]+)_phys_(\d+)_eng_(\d+)_engtype_(.+)$', 'IgnoreCase')
    if (!$match.Success) { return $null }
    [pscustomobject]@{pid=[int]$match.Groups[1].Value; luid=$match.Groups[2].Value.ToLowerInvariant();
        phys=[int]$match.Groups[3].Value; engine=[int]$match.Groups[4].Value; type=$match.Groups[5].Value.ToLowerInvariant()}
}
function Cpu-Usage([double]$CpuSeconds, [double]$WallSeconds, [int]$LogicalProcessors) {
    if ($WallSeconds -le 0 -or $LogicalProcessors -le 0) { throw 'CPU denominator must be positive.' }
    [pscustomobject]@{cores=$CpuSeconds/$WallSeconds; machine_percent=100*$CpuSeconds/$WallSeconds/$LogicalProcessors}
}
function Gpu-Summary($Rows, [int]$TargetId, [string]$PreferredLuid = '') {
    $engines = @{}
    $target = $null
    foreach ($row in $Rows) {
        if ($row.status -ne 0 -and $row.status -ne 1) { continue }
        if ([double]::IsNaN($row.percent) -or [double]::IsInfinity($row.percent)) { continue }
        $key = "$($row.luid)/$($row.phys)/$($row.engine)"
        if (!$engines.ContainsKey($key)) { $engines[$key] = 0.0 }
        $engines[$key] += $row.percent
        if ($row.pid -eq $TargetId -and (!$PreferredLuid -or $row.luid -eq $PreferredLuid) -and ($null -eq $target -or $row.percent -gt $target.percent)) { $target = $row }
    }
    $whole = $null
    $selectedLuid = if ($PreferredLuid) {$PreferredLuid} elseif ($null -ne $target) {$target.luid} else {''}
    if ($selectedLuid) {
        foreach ($key in $engines.Keys) {
            if ($key.StartsWith($selectedLuid + '/') -and ($null -eq $whole -or $engines[$key] -gt $whole)) { $whole = $engines[$key] }
        }
    }
    [pscustomobject]@{luid=$selectedLuid;
        process_percent=$(if ($null -ne $target) {$target.percent} else {$null}); whole_same_adapter_percent=$whole}
}
function Counter-ErrorKind($Record, [string]$InvalidSampleMessage, [string]$MoreDataMessage = '') {
    if ($Record.FullyQualifiedErrorId -like 'CounterApiError*' -and $Record.Exception.Message -eq $InvalidSampleMessage) { return 'invalid_sample' }
    if ($Record.FullyQualifiedErrorId -like 'CounterApiError*' -and $MoreDataMessage -and $Record.Exception.Message.Trim() -eq $MoreDataMessage.Trim()) { return 'retry_more_data' }
    return 'unexpected_error'
}
function Duration-Reached([double]$Elapsed, [int]$Limit) { return $Limit -gt 0 -and $Elapsed -ge $Limit }
function Gpu-CounterStartState([string]$ReadyFile, [bool]$GpuEnabled, [bool]$TargetAlive,
    [bool]$OwnerStopped, [double]$Elapsed, [int]$Limit) {
    if ($OwnerStopped) { return 'owner_stop' }
    if (!$TargetAlive) { return 'process_exit' }
    if (Duration-Reached $Elapsed $Limit) { return 'max_seconds' }
    if (!$GpuEnabled -or !$ReadyFile) { return 'ready' }
    try {
        if ([IO.File]::Exists($ReadyFile) -and [IO.FileInfo]::new($ReadyFile).Length -gt 0) { return 'ready' }
    } catch [IO.IOException] { } # Creation/replacement can race this length check; retry within the same bounds.
    return 'waiting_gpu_ready'
}
function Target-Alive($Process, [long]$StartTicks) {
    $Process.Refresh()
    return !$Process.HasExited -and $Process.StartTime.ToUniversalTime().Ticks -eq $StartTicks
}
function Csv-Row($Values) {
    return (@($Values | ForEach-Object {
        $value = if ($null -eq $_) {''} elseif ($_ -is [IFormattable]) {$_.ToString($null,$culture)} else {[string]$_}
        if ($value.IndexOfAny([char[]]",`"`r`n") -ge 0) {'"' + $value.Replace('"','""') + '"'} else {$value}
    }) -join ',')
}
function Local-CounterName([string]$English) {
    if ($counterNames.ContainsKey($English)) { return $counterNames[$English] }
    return $English
}
function Record-Issue([string]$Component, [string]$Kind, [string]$Message) {
    $entry = [pscustomobject]@{utc=[DateTime]::UtcNow.ToString('o');qpc=[Diagnostics.Stopwatch]::GetTimestamp();component=$Component;kind=$Kind;message=$Message}
    $writers['sampling-errors'].WriteLine(($entry | ConvertTo-Json -Compress))
    $writers['sampling-errors'].Flush()
}
function Write-SamplingMetadata([bool]$Complete) {
    $metadata = [ordered]@{complete=$Complete;process_id=$ProcessId}
    if ($null -ne $failure) { $metadata.error=$failure }
    if ($CounterWorker) {
        $metadata.counter_worker_process_id=$PID
        if ($counterErrorCount -gt 0) { $metadata.counter_errors_total=$counterErrorCount }
    }
    if (!$Complete -or $null -ne $failure -or ($CounterWorker -and $counterErrorCount -gt 0)) {
        $metadata.reason=$reason
    }
    if (!$CounterWorker -and $NvidiaSmi) {
        $nvidia = $null
        if (Test-Path -LiteralPath $nvidiaStatusPath) {
            try { $nvidia=Read-SharedJson $nvidiaStatusPath } catch {}
        }
        $nvidiaSummary = $null
        if ($null -ne $nvidia) {
            $nvidiaSummary = [ordered]@{complete=$nvidia.complete}
            if ($null -ne $nvidia.PSObject.Properties['error'] -and $null -ne $nvidia.error) { $nvidiaSummary.error=$nvidia.error }
        }
        $metadata.nvidia_worker=$nvidiaSummary
    }
    $path=if ($CounterWorker) {$pdhStatusPath} else {Join-Path $outputRoot 'sampling.json'}
    Write-AtomicJson $metadata $path
}


function Read-SharedJson([string]$Path) {
    # Permit atomic replacement while reading the old complete snapshot.
    for ($attempt=0;$attempt -lt 5;++$attempt) {
        try {
            $share=[IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
            $stream=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,$share)
            $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8)
            try { return ($reader.ReadToEnd() | ConvertFrom-Json) } finally { $reader.Dispose() }
        } catch [IO.IOException] {
            if ($attempt -eq 4) { throw }
            Start-Sleep -Milliseconds 20
        }
    }
}
function Finish-CounterWorker($Worker, [string]$StatusPath) {
    # A 1 Hz query still has to return its next sample, close PDH and publish its receipt.
    # Keep startup/enumeration stalls bounded, but do not kill normal finalization at 1 second.
    $stoppedByOwner = !$Worker.WaitForExit(5000)
    $exited = $true
    $errorMessage = $null
    if ($stoppedByOwner) {
        $Worker.Kill()
        $exited = $Worker.WaitForExit(1000)
        $errorMessage = 'Owned PDH worker exceeded its five-second finalization grace; final counters remain incomplete.'
    } elseif ($Worker.ExitCode -ne 0) {
        $errorMessage = 'Owned PDH worker failed; inspect pdh-worker.json, pdh-errors.jsonl and stderr.'
    } else {
        try {
            $receipt = Read-SharedJson $StatusPath
            if (!$receipt.complete -or $receipt.counter_worker_process_id -ne $Worker.Id) {
                $errorMessage = 'Owned PDH worker exited without its final completion receipt.'
            } elseif ($null -ne $receipt.PSObject.Properties['error'] -and $null -ne $receipt.error) {
                $errorMessage = 'Owned PDH worker reported an error; inspect pdh-worker.json and pdh-errors.jsonl.'
            }
        } catch {
            $errorMessage = 'Cannot read owned PDH worker final completion receipt: ' + $_.Exception.Message
        }
    }
    [pscustomobject]@{stopped_by_owner=$stoppedByOwner;exited=$exited;error=$errorMessage}
}
function Counter-QueryRefresh([bool]$GpuEnabled, [bool]$TargetObserved, [int]$QuerySamples, [double]$QuerySeconds) {
    # Get-Counter expands wildcard instances once per query, so refresh after delayed D3D creation
    # and periodically discover new engines/PIDs. CPU sampling runs independently of enumeration.
    return $GpuEnabled -and ((!$TargetObserved -and $QuerySamples -ge 2) -or $QuerySeconds -ge 60)
}
function Counter-SummaryReady($Snapshot, [long]$LastSample, [long]$CurrentQpc) {
    return $null -ne $Snapshot -and $Snapshot.sample -gt $LastSample -and $Snapshot.warmup -eq 0 -and $Snapshot.qpc -le $CurrentQpc
}
function Write-AtomicJson($Data, [string]$Path) {
    $temporary=$Path+'.tmp'
    [IO.File]::WriteAllText($temporary,($Data | ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
    for ($attempt=0;$attempt -lt 5;++$attempt) {
        try {
            if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary,$Path,[System.Management.Automation.Language.NullString]::Value) } else { [IO.File]::Move($temporary,$Path) }
            return
        } catch [IO.IOException] {
            if ($attempt -eq 4) { throw }
            Start-Sleep -Milliseconds 20
        }
    }
}
function Invoke-OwnerSampler {
    # Native GPU Engine enumeration can block for tens of seconds. The exact owned hidden child
    # performs PDH while process/thread CPU and PID/deadline checks remain independent at 1 Hz.
    $pdhWorker=$null
    try {
        foreach ($name in @('cpu-process','cpu-threads','sampling-errors')) {
            $filename=$name+$(if ($name -eq 'sampling-errors') {'.jsonl'} else {'.csv'})
            $writers[$name]=[IO.StreamWriter]::new((Join-Path $outputRoot $filename),$false,[Text.UTF8Encoding]::new($false))
        }
        $writers['cpu-process'].WriteLine('sample,utc,qpc,qpc_frequency,warmup,wall_seconds,cpu_seconds,core_equivalents,machine_percent,logical_processors,threads_sampled,thread_errors,top_thread_id,top_thread_core_equivalents,target_gpu_luid,process_gpu_busiest_engine_percent,whole_gpu_same_adapter_busiest_engine_percent,system_cpu_percent,system_busy_core_equivalents,working_set_bytes,private_memory_bytes,handle_count,counter_sample,counter_qpc,counter_age_seconds,counter_ready')
        $writers['cpu-threads'].WriteLine('sample,utc,qpc,thread_id,cpu_seconds,core_equivalents,new_thread')
        foreach ($writer in $writers.Values) { $writer.Flush() }
    if ($NvidiaSmi) {
        $smi=Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
        if ($null -eq $smi) { Record-Issue 'nvidia' 'unavailable' 'nvidia-smi.exe was not found; NVIDIA fields are unavailable.' }
        else {
        $nvidiaJob=Start-Job -ArgumentList $smi.Source,(Join-Path $outputRoot 'nvidia-whole-gpu.csv'),$ProcessId,$targetStartTime.Ticks,$MaxSeconds,$nvidiaStopPath,$nvidiaStatusPath,$nvidiaChildPath -ScriptBlock {
            param($Smi,$Csv,$GameId,$GameStartTicks,$Seconds,$StopFile,$StatusFile,$ChildFile)
            $ErrorActionPreference='Stop'
            $clock=[Diagnostics.Stopwatch]::StartNew()
            $writer=[IO.StreamWriter]::new($Csv,$false,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
            $errors=[IO.StreamWriter]::new(($Csv + '.errors.jsonl'),$false,[Text.UTF8Encoding]::new($false));$errors.AutoFlush=$true
            $writer.WriteLine('sample,utc,qpc,qpc_frequency,nvidia_timestamp,index,uuid,pci_bus_id,name,pstate,gpu_percent,memory_controller_percent,graphics_clock_mhz,sm_clock_mhz,memory_clock_mhz,memory_used_mib,memory_total_mib,memory_free_mib,temperature_c,power_watts,power_limit_watts')
            $samples=0;$workerError=$null;$child=$null;$workerReason='running'
            function Alive {
                if ([IO.File]::Exists($StopFile) -or ($Seconds -gt 0 -and $clock.Elapsed.TotalSeconds -ge $Seconds)) { return $false }
                $game=Get-Process -Id $GameId -ErrorAction SilentlyContinue
                if ($null -eq $game) { return $false }
                try { return !$game.HasExited -and $game.StartTime.ToUniversalTime().Ticks -eq $GameStartTicks }
                catch { if ($game.HasExited) { return $false }; throw }
            }
            try {
                while (Alive) {
                    $sampleStart=$clock.Elapsed.TotalSeconds
                    $child=[Diagnostics.Process]::new()
                    $child.StartInfo.FileName=$Smi
                    $child.StartInfo.Arguments='--query-gpu=timestamp,index,uuid,pci.bus_id,name,pstate,utilization.gpu,utilization.memory,clocks.current.graphics,clocks.current.sm,clocks.current.memory,memory.used,memory.total,memory.free,temperature.gpu,power.draw,power.limit --format=csv,noheader,nounits'
                    $child.StartInfo.UseShellExecute=$false;$child.StartInfo.CreateNoWindow=$true
                    $child.StartInfo.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
                    $child.StartInfo.RedirectStandardOutput=$true;$child.StartInfo.RedirectStandardError=$true
                    $null=$child.Start()
                    @{pid=$child.Id;start_ticks=$child.StartTime.ToUniversalTime().Ticks;path=$Smi} | ConvertTo-Json | Set-Content -LiteralPath $ChildFile -Encoding UTF8
                    $stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync()
                    while (!$child.WaitForExit(100)) {
                        if (!(Alive) -or $clock.Elapsed.TotalSeconds-$sampleStart -ge 2) {
                            $child.Kill();$child.WaitForExit();break
                        }
                    }
                    $text=$stdout.GetAwaiter().GetResult();$errorText=$stderr.GetAwaiter().GetResult()
                    if ($errorText.Trim()) { $errors.WriteLine((@{utc=[DateTime]::UtcNow.ToString('o');kind='stderr';exit_code=$child.ExitCode;message=$errorText} | ConvertTo-Json -Compress)) }
                    if (!(Alive)) { $workerReason='target_exit_or_stop';break }
                    if ($child.ExitCode -ne 0) { throw "nvidia-smi exited $($child.ExitCode): $errorText" }
                    ++$samples
                    $prefix="$samples,$([DateTime]::UtcNow.ToString('o')),$([Diagnostics.Stopwatch]::GetTimestamp()),$([Diagnostics.Stopwatch]::Frequency),"
                    foreach ($line in ($text -split '\r?\n')) { if ($line.Trim()) { $writer.WriteLine($prefix+$line) } }
                    $child.Dispose();$child=$null
                    if ([IO.File]::Exists($ChildFile)) { [IO.File]::Delete($ChildFile) }
                    $remaining=1.0-($clock.Elapsed.TotalSeconds-$sampleStart)
                    if ($remaining -gt 0) { Start-Sleep -Milliseconds ([int](1000*$remaining)) }
                }
                if ($workerReason -eq 'running') { $workerReason='target_exit_or_limit' }
            } catch {
                $workerError=$_.Exception.Message;$workerReason='error'
                $errors.WriteLine((@{utc=[DateTime]::UtcNow.ToString('o');message=$workerError} | ConvertTo-Json -Compress))
            } finally {
                if ($null -ne $child) { if (!$child.HasExited) {$child.Kill();$child.WaitForExit()};$child.Dispose() }
                if ([IO.File]::Exists($ChildFile)) { [IO.File]::Delete($ChildFile) }
                $writer.Dispose();$errors.Dispose()
                @{complete=$true;samples=$samples;reason=$workerReason;error=$workerError;whole_device_includes_other_apps=$true} | ConvertTo-Json | Set-Content -LiteralPath $StatusFile -Encoding UTF8
            }
        }
        }
    }

        $shell=(Get-Command powershell.exe -ErrorAction Stop).Source
        $workerArguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSCommandPath+'"'),'-CounterWorker',
            '-ProcessId',$ProcessId,'-ProcessStartTicks',$targetStartTime.Ticks,'-MaxSeconds',$MaxSeconds,
            '-OutputDirectory',('"'+$outputRoot+'"'))
        if ($NoGpuCounters) { $workerArguments+='-NoGpuCounters' }
        if ($GpuReadyFile) { $workerArguments+=@('-GpuReadyFile',('"'+$GpuReadyFile+'"')) }
        if ($AdapterLuid) { $workerArguments+=@('-AdapterLuid',$AdapterLuid) }
        $pdhWorker=Start-Process -FilePath $shell -WindowStyle Hidden -ArgumentList $workerArguments -PassThru -RedirectStandardError (Join-Path $outputRoot 'pdh-worker-stderr.log')
        $null=$pdhWorker.Handle
        Write-SamplingMetadata $false
        $lastCounterSample=0
        $nextTick=$startQpc+$qpcFrequency
        while ($true) {
            if (!(Target-Alive $targetProcess $targetStartTime.Ticks)) { $reason='process_exit';break }
            $qpc=[Diagnostics.Stopwatch]::GetTimestamp()
            if (Duration-Reached (($qpc-$startQpc)/$qpcFrequency) $MaxSeconds) { $reason='max_seconds';break }
            if ($qpc -lt $nextTick) { Start-Sleep -Milliseconds 100;continue }
            $nextTick=$qpc+$qpcFrequency
            ++$sample
            $utc=[DateTime]::UtcNow.ToString('o');$warmup=[int]($sample -eq 1)
            $counterSample='';$counterQpc='';$counterAge='';$counterReady=0;$counterLuid=''
            $processGpu='';$wholeGpu='';$systemPercent='';$systemCores=''
            if ([IO.File]::Exists($pdhLatestPath)) {
                $latest=Read-SharedJson $pdhLatestPath
                if ($latest.sample -gt $lastCounterSample -and $latest.qpc -le $qpc) {
                    $counterSample=$latest.sample;$counterQpc=$latest.qpc;$counterReady=[int](Counter-SummaryReady $latest $lastCounterSample $qpc)
                    $counterAge=(($qpc-$latest.qpc)/$qpcFrequency).ToString('R',$culture)
                    $counterLuid=$latest.luid;$lastCounterSample=$latest.sample
                    if ($counterReady -and $null -ne $latest.process_percent) { $processGpu=([double]$latest.process_percent).ToString('R',$culture) }
                    if ($counterReady -and $null -ne $latest.whole_same_adapter_percent) { $wholeGpu=([double]$latest.whole_same_adapter_percent).ToString('R',$culture) }
                    if ($counterReady -and ![string]::IsNullOrWhiteSpace([string]$latest.system_percent)) {
                        $systemPercent=([double]$latest.system_percent).ToString('R',$culture)
                        $systemCores=([double]$latest.system_percent*$logicalProcessors/100.0).ToString('R',$culture)
                    }
                }
            }
            $wall = ($qpc - $lastQpc) / $qpcFrequency
            $totalCpu = $targetProcess.TotalProcessorTime.TotalSeconds
            [double]$cpuSeconds = [Math]::Max(0.0, $totalCpu - $lastCpu)
            $cpu = Cpu-Usage $cpuSeconds $wall $logicalProcessors
            $nextThreads = @{}
            $threadErrors = 0
            $topThread = 0
            $topThreadCores = 0.0
            foreach ($thread in $targetProcess.Threads) {
                try {
                    $total = $thread.TotalProcessorTime.TotalSeconds
                    $newThread = !$threadTotals.ContainsKey($thread.Id) -or $total -lt $threadTotals[$thread.Id]
                    $previous = if ($newThread) {0.0} else {$threadTotals[$thread.Id]}
                    [double]$delta = [Math]::Max(0.0, $total - $previous)
                    $cores = $delta / $wall
                    $nextThreads[$thread.Id] = $total
                    if ($cores -gt $topThreadCores) { $topThread = $thread.Id; $topThreadCores = $cores }
                    $writers['cpu-threads'].WriteLine((@($sample,$utc,$qpc,$thread.Id,$delta.ToString('R',$culture),$cores.ToString('R',$culture),[int]$newThread) -join ','))
                } catch { ++$threadErrors; Record-Issue 'thread' 'read_failed' $_.Exception.Message }
            }

            $writers['cpu-process'].WriteLine((@($sample,$utc,$qpc,$qpcFrequency,$warmup,$wall.ToString('R',$culture),$cpuSeconds.ToString('R',$culture),$cpu.cores.ToString('R',$culture),$cpu.machine_percent.ToString('R',$culture),$logicalProcessors,$nextThreads.Count,$threadErrors,$topThread,$topThreadCores.ToString('R',$culture),$counterLuid,$processGpu,$wholeGpu,$systemPercent,$systemCores,$targetProcess.WorkingSet64,$targetProcess.PrivateMemorySize64,$targetProcess.HandleCount,$counterSample,$counterQpc,$counterAge,$counterReady) -join ','))
            foreach ($writer in $writers.Values) { $writer.Flush() }
            $threadTotals = $nextThreads
            $lastCpu = $totalCpu
            $lastQpc = $qpc
            if ($sample % 10 -eq 0) { Write-SamplingMetadata $false }

        }
    } catch {
        if (!(Target-Alive $targetProcess $targetStartTime.Ticks)) { $reason='process_exit' }
        else { $reason='error';$failure=$_.Exception.Message }
    } finally {
        [IO.File]::WriteAllText($pdhStopPath,'stop')
        if ($null -ne $pdhWorker) {
            $shutdown=Finish-CounterWorker $pdhWorker $pdhStatusPath
            $pdhStoppedByOwner=$shutdown.stopped_by_owner
            if (!$shutdown.exited) { Record-Issue 'pdh' 'owned_worker_cleanup_failed' 'Owned PDH worker did not exit after termination.' }
            if ($pdhStoppedByOwner) { Record-Issue 'pdh' 'owner_stop' $shutdown.error }
            if ($null -ne $shutdown.error) {
                if ($null -eq $failure) { $failure=$shutdown.error }
                Record-Issue 'pdh' 'finalization_failed' $shutdown.error
            }
            $pdhWorker.Dispose()
        }
        [IO.File]::WriteAllText($nvidiaStopPath,'stop')
        if ($null -ne $nvidiaJob) {
            $null=Wait-Job -Job $nvidiaJob -Timeout 3
            if ($nvidiaJob.State -eq 'Running') {
                Stop-Job -Job $nvidiaJob
                if (Test-Path -LiteralPath $nvidiaChildPath) {
                    try {
                        $owned=Get-Content -LiteralPath $nvidiaChildPath -Raw | ConvertFrom-Json
                        $child=Get-Process -Id $owned.pid -ErrorAction SilentlyContinue
                        if ($null -ne $child -and $child.StartTime.ToUniversalTime().Ticks -eq $owned.start_ticks -and $child.Path -eq $owned.path) { $child.Kill() }
                    } catch { Record-Issue 'nvidia' 'owned_child_cleanup_failed' $_.Exception.Message }
                }
                Record-Issue 'nvidia' 'worker_timeout' 'Owned NVIDIA worker did not stop within three seconds.'
            }
            Receive-Job -Job $nvidiaJob -ErrorAction Continue 2>&1 | Out-String | Set-Content -LiteralPath (Join-Path $outputRoot 'nvidia-worker.log') -Encoding UTF8
            Remove-Job -Job $nvidiaJob -Force
        }
        if ($null -ne $failure) { Record-Issue 'sampler' 'unexpected_error' $failure }
        Write-SamplingMetadata $true
        foreach ($writer in $writers.Values) { $writer.Dispose() }
    }
    if ($null -ne $failure) { throw $failure }
}

if ($ProcessId -le 0 -or [string]::IsNullOrWhiteSpace($OutputDirectory)) { throw 'ProcessId and OutputDirectory are required.' }
$targetProcess = Get-Process -Id $ProcessId
$targetStartTime = $targetProcess.StartTime.ToUniversalTime()
if ($ProcessStartTicks -and $targetStartTime.Ticks -ne $ProcessStartTicks) { throw 'Target PID/start time differs from the requested process.' }
if ($AdapterLuid) { $AdapterLuid=$AdapterLuid.ToLowerInvariant() }
if ($GpuReadyFile) { $GpuReadyFile=[IO.Path]::GetFullPath($GpuReadyFile) }
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$logicalProcessors = [Environment]::ProcessorCount
$qpcFrequency = [Diagnostics.Stopwatch]::Frequency
$startQpc = [Diagnostics.Stopwatch]::GetTimestamp()
$startUtc = [DateTime]::UtcNow
$lastQpc = $startQpc
$lastCpu = $targetProcess.TotalProcessorTime.TotalSeconds
$threadTotals = @{}
foreach ($thread in $targetProcess.Threads) {
    try { $threadTotals[$thread.Id] = $thread.TotalProcessorTime.TotalSeconds } catch {}
}
$writers = @{}
$nvidiaJob = $null
$sample = 0
$reason = 'running'
$failure = $null
$counterErrorCount = 0
$nvidiaStatusPath = Join-Path $outputRoot 'nvidia-worker.json'
$nvidiaStopPath = Join-Path $outputRoot '.nvidia-stop'
$nvidiaChildPath = Join-Path $outputRoot '.nvidia-child.json'
if (Test-Path -LiteralPath $nvidiaStopPath) { Remove-Item -LiteralPath $nvidiaStopPath }
$pdhStatusPath=Join-Path $outputRoot 'pdh-worker.json'
$pdhLatestPath=Join-Path $outputRoot 'pdh-latest.json'
$pdhStopPath=Join-Path $outputRoot '.pdh-stop'
$pdhStoppedByOwner=$false
$pdhRetries=0
$gpuReadyWaitState=if ($CounterWorker) {'not_checked'} else {'worker_pending'}
if (!$CounterWorker) {
    if (Test-Path -LiteralPath $pdhStopPath) { Remove-Item -LiteralPath $pdhStopPath }
    Invoke-OwnerSampler
    exit 0
}
$null = Get-Command Get-Counter
$counterAssembly = [Microsoft.PowerShell.Commands.GetCounterCommand].Assembly
$counterResourceName = @($counterAssembly.GetManifestResourceNames() | Where-Object { $_.EndsWith('GetEventResources.resources') })[0]
$counterResources = [Resources.ResourceManager]::new($counterResourceName.Substring(0, $counterResourceName.Length - '.resources'.Length), $counterAssembly)
$invalidSampleMessage = $counterResources.GetString('CounterSampleDataInvalid')
$pdhMessageArguments = @([uint32]2147485650, 'pdh.dll', '') # PDH_MORE_DATA, enumeration can grow during startup.
$pdhMessageMethod = $counterAssembly.GetType('Microsoft.PowerShell.Commands.Diagnostics.Common.CommonUtilities').GetMethod('FormatMessageFromModule', [Reflection.BindingFlags]'Static,Public,NonPublic')
if ($pdhMessageMethod.Invoke($null, $pdhMessageArguments) -ne 0) { throw 'Cannot resolve the PDH_MORE_DATA diagnostic.' }
$moreDataMessage = [string]$pdhMessageArguments[2]
$pdhRetries = 0
$counterNames = @{}
try {
    $english=(Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Perflib\009' -Name Counter).Counter
    $local=(Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Perflib\CurrentLanguage' -Name Counter).Counter
    $byIndex=@{}
    for ($index=0;$index -lt $local.Count-1;$index+=2) { $byIndex[$local[$index]]=$local[$index+1] }
    for ($index=0;$index -lt $english.Count-1;$index+=2) {
        if ($byIndex.ContainsKey($english[$index])) { $counterNames[$english[$index+1]]=$byIndex[$english[$index]] }
    }
} catch { $counterNames=@{} }
$cpuObject=Local-CounterName 'Processor Information'
$cpuMetric=Local-CounterName '% Processor Time'
$gpuObject=Local-CounterName 'GPU Engine'
$gpuMetric=Local-CounterName 'Utilization Percentage'
$counterPaths=@("\$cpuObject(*)\$cpuMetric")
if (!$NoGpuCounters) { $counterPaths += "\$gpuObject(*)\$gpuMetric" }
try {
    foreach ($name in @('gpu-engines', 'cpu-system', 'sampling-errors')) {
        $writers[$name] = [IO.StreamWriter]::new((Join-Path $outputRoot $(if ($name -eq 'sampling-errors') {'pdh-errors.jsonl'} else {$name+'.csv'})), $false, [Text.UTF8Encoding]::new($false))
    }
    $writers['gpu-engines'].WriteLine('sample,utc,qpc,qpc_frequency,warmup,pid,luid,phys,engine,engine_type,utilization_percent,status,target_process')
    $writers['cpu-system'].WriteLine('sample,utc,qpc,qpc_frequency,warmup,instance,utilization_percent,status')
    foreach ($writer in $writers.Values) { $writer.Flush() }
    Write-SamplingMetadata $false
    while ($true) {
        $readyQpc=[Diagnostics.Stopwatch]::GetTimestamp()
        $gpuReadyWaitState=Gpu-CounterStartState $GpuReadyFile (!$NoGpuCounters) (Target-Alive $targetProcess $targetStartTime.Ticks) ([IO.File]::Exists($pdhStopPath)) (($readyQpc-$startQpc)/$qpcFrequency) $MaxSeconds
        if ($gpuReadyWaitState -eq 'ready') {
            Write-SamplingMetadata $false
            break
        }
        if ($gpuReadyWaitState -ne 'waiting_gpu_ready') {
            $reason=$gpuReadyWaitState
            throw [OperationCanceledException]::new($reason)
        }
        if ($reason -ne 'waiting_gpu_ready') { $reason='waiting_gpu_ready';Write-SamplingMetadata $false }
        Start-Sleep -Milliseconds 100
    }
    $reason='running'
    while ($true) {
    $retryPdh = $false
    $refreshPdh = $false
    $querySamples=0
    $queryStartQpc=[Diagnostics.Stopwatch]::GetTimestamp()
    Write-SamplingMetadata $false
    try {
    Get-Counter -Counter $counterPaths -SampleInterval 1 -Continuous -ErrorAction Continue 2>&1 | ForEach-Object {
        if (!(Target-Alive $targetProcess $targetStartTime.Ticks)) { $reason = 'process_exit'; throw [OperationCanceledException]::new($reason) }
        if ([IO.File]::Exists($pdhStopPath)) { $reason='owner_stop';throw [OperationCanceledException]::new($reason) }
        $qpc = [Diagnostics.Stopwatch]::GetTimestamp()
        if (Duration-Reached (($qpc - $startQpc) / $qpcFrequency) $MaxSeconds) { $reason = 'max_seconds'; throw [OperationCanceledException]::new($reason) }
        if ($_ -is [Management.Automation.ErrorRecord]) {
            $counterRecord = $_
            ++$counterErrorCount
            Record-Issue 'pdh' $_.FullyQualifiedErrorId $_.Exception.Message
            switch (Counter-ErrorKind $counterRecord $invalidSampleMessage $moreDataMessage) {
                'invalid_sample' { return }
                'retry_more_data' { $retryPdh=$true; throw [InvalidOperationException]::new('Reopen PDH query after enumeration buffer growth.') }
                default { throw $counterRecord }
            }
        }
        ++$sample
        ++$querySamples
        $utc = $_.Timestamp.ToUniversalTime().ToString('o')
        $warmup = [int]($querySamples -eq 1)
        $rows = @($_.CounterSamples | ForEach-Object {
            $parsed = Parse-GpuInstance $_.InstanceName
            if ($null -ne $parsed) {
                [pscustomobject]@{pid=$parsed.pid;luid=$parsed.luid;phys=$parsed.phys;engine=$parsed.engine;type=$parsed.type;percent=[double]$_.CookedValue;status=[long]$_.Status}
            }
        })
        $targetObserved=$false
        foreach ($row in $rows) {
            if ($row.pid -eq $ProcessId) { $targetObserved=$true }
            $writers['gpu-engines'].WriteLine((@($sample,$utc,$qpc,$qpcFrequency,$warmup,$row.pid,$row.luid,$row.phys,$row.engine,$row.type,$row.percent.ToString('R',$culture),$row.status,[int]($row.pid -eq $ProcessId)) -join ','))
        }
        $systemPercent=$null
        foreach ($counter in $_.CounterSamples) {
            if ($counter.Path -notlike "*\$cpuObject(*)\$cpuMetric") { continue }
            $writers['cpu-system'].WriteLine((Csv-Row @($sample,$utc,$qpc,$qpcFrequency,$warmup,$counter.InstanceName,$counter.CookedValue,$counter.Status)))
            if ($counter.InstanceName -eq '_total' -and ($counter.Status -eq 0 -or $counter.Status -eq 1) -and
                ![double]::IsNaN($counter.CookedValue) -and ![double]::IsInfinity($counter.CookedValue)) { $systemPercent=[double]$counter.CookedValue }
        }
        $systemCores=if ($null -ne $systemPercent) {($systemPercent*$logicalProcessors/100.0).ToString('R',$culture)} else {''}
        $systemPercent=if ($null -ne $systemPercent) {$systemPercent.ToString('R',$culture)} else {''}
        $gpu = Gpu-Summary $rows $ProcessId $AdapterLuid
        $processGpu = if ($null -ne $gpu.process_percent) {$gpu.process_percent.ToString('R',$culture)} else {''}
        $wholeGpu = if ($null -ne $gpu.whole_same_adapter_percent) {$gpu.whole_same_adapter_percent.ToString('R',$culture)} else {''}
        Write-AtomicJson ([ordered]@{sample=$sample;utc=$utc;qpc=$qpc;qpc_frequency=$qpcFrequency;warmup=$warmup;
            luid=$gpu.luid;process_percent=$gpu.process_percent;whole_same_adapter_percent=$gpu.whole_same_adapter_percent;
            system_percent=$systemPercent}) $pdhLatestPath
        foreach ($writer in $writers.Values) { $writer.Flush() }
        if ($sample -eq 1 -or $sample % 10 -eq 0) { Write-SamplingMetadata $false }
        if (Counter-QueryRefresh (!$NoGpuCounters) $targetObserved $querySamples (($qpc-$queryStartQpc)/$qpcFrequency)) {
            $refreshPdh=$true
            Write-SamplingMetadata $false
            throw [InvalidOperationException]::new('Refresh PDH wildcard instances.')
        }
    }
    } catch [InvalidOperationException] {
        if (!$retryPdh -and !$refreshPdh) { throw }
    }
    if (!$retryPdh -and !$refreshPdh) { break }
    if ($retryPdh) { ++$pdhRetries }
    if ($pdhRetries -gt 5) { throw 'PDH instance enumeration kept growing after five query retries.' }
    $targetProcess.Refresh()
    if ($targetProcess.HasExited) { $reason='process_exit'; throw [OperationCanceledException]::new($reason) }
    if (Duration-Reached (([Diagnostics.Stopwatch]::GetTimestamp()-$startQpc)/$qpcFrequency) $MaxSeconds) { $reason='max_seconds'; throw [OperationCanceledException]::new($reason) }
    Start-Sleep -Milliseconds 100
    }
} catch [OperationCanceledException] {
    # Normal bounded exit from the persistent PDH query.
} catch {
    $targetProcess.Refresh()
    if ($targetProcess.HasExited) { $reason = 'process_exit' }
    else { $reason = 'error'; $failure = $_.Exception.Message }
} finally {
    if ($null -ne $failure) { Record-Issue 'sampler' 'unexpected_error' $failure }
    Write-SamplingMetadata $true
    foreach ($writer in $writers.Values) { $writer.Dispose() }
}

if ($null -ne $failure) { throw $failure }
