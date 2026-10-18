param([string]$PrototypeRoot=(Join-Path (Split-Path $PSScriptRoot -Parent) 'Build/pass-optimization-20261018/ao-guide-sharing'),[switch]$Warp)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$out=Join-Path $PrototypeRoot 'gpu-test'
New-Item -ItemType Directory -Force $out | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$dev=Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$lines=& $env:ComSpec /d /c "`"$dev`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
if($LASTEXITCODE -ne 0){throw 'MSVC environment failed'}
foreach($line in $lines){if($line -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$env:VSLANG='1033'
$dxcLine=Get-Content (Join-Path $repo 'Build/msvc-validation/CMakeCache.txt') | Where-Object { $_ -match '^HS_DXC:FILEPATH=' } | Select-Object -First 1
if(!$dxcLine){throw 'Configured HS_DXC is unavailable'}
$dxc=$dxcLine.Substring('HS_DXC:FILEPATH='.Length)
$compiled=Join-Path $out 'compiled'
foreach($version in @('before','after')){
    $source=Join-Path $PrototypeRoot "$version/adaptive_gtao.hlsl"
    $dest=Join-Path $compiled $version
    New-Item -ItemType Directory -Force $dest | Out-Null
    foreach($entry in @('AdaptiveGtaoHorizonCS','AdaptiveGtaoResolveCS')){
        & $dxc -T cs_6_0 -E $entry -O3 -Ges -WX -Fo (Join-Path $dest "$entry.dxil") $source
        if($LASTEXITCODE -ne 0){throw "DXC failed: $version/$entry"}
    }
}
$sourceFiles=foreach($version in @('before','after')){Get-Item (Join-Path $PrototypeRoot "$version/adaptive_gtao.hlsl"),(Join-Path $PrototypeRoot "$version/ambient_occlusion.hlsl")}
$hashes=@($sourceFiles; Get-ChildItem $compiled -Filter '*.dxil' -Recurse) | Get-FileHash -Algorithm SHA256
$hashes | Select-Object Path,Hash | ConvertTo-Json | Set-Content (Join-Path $out 'inputs.json') -Encoding utf8
& cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /utf-8 (Join-Path $PSScriptRoot 'ao_guide_equivalence_gpu.cpp') "/Fo$out/ao_guide_equivalence_gpu.obj" "/Fe$out/ao_guide_equivalence_gpu.exe" /link d3d12.lib dxgi.lib *> (Join-Path $out 'build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out 'build.log');exit $LASTEXITCODE}
Copy-Item -LiteralPath (Join-Path $repo 'Build/msvc-validation/D3D12') -Destination $out -Recurse -Force
$mode=if($Warp){'warp'}else{'hardware'}
& (Join-Path $out 'ao_guide_equivalence_gpu.exe') $compiled $mode *> (Join-Path $out "$mode.log")
$code=$LASTEXITCODE
Get-Content (Join-Path $out "$mode.log") -Tail 30
exit $code
