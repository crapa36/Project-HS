param([Parameter(Mandatory=$true)][string]$Cooked)
$ErrorActionPreference='Stop'
$sets=@(@('terrain',4,12,@('basecolor','surface','normal','detail','height')),@('rock',4,12,@('basecolor','surface','normal','detail')),@('bark',3,12,@('basecolor','surface','normal','detail')),@('grass',2,6,@('basecolor','normal','roughness')),@('leaf',3,6,@('basecolor','normal','roughness')))
$count=0
foreach($set in $sets){ foreach($role in $set[3]){
    $file=Join-Path $Cooked "environment_$($set[0])_$role.dds"
    $bytes=[IO.File]::ReadAllBytes($file)
    if($bytes.Length -lt 148){throw "Truncated DDS: $file"}
    $format=if($role -eq 'basecolor'){99}elseif($role -in @('height','roughness')){80}else{83}
    foreach($check in @(@(0,0x20534444),@(4,124),@(12,2048),@(16,2048),@(28,$set[2]),@(76,32),@(84,0x30315844),@(128,$format),@(132,3),@(140,$set[1]))){
        if([BitConverter]::ToUInt32($bytes,$check[0]) -ne $check[1]){throw "DDS header mismatch at $($check[0]): $file"}
    }
    $block=if($format -eq 80){8}else{16};[long]$expected=148
    for($mip=0;$mip -lt $set[2];++$mip){$edge=[Math]::Max(1,2048 -shr $mip);$blocks=[Math]::Ceiling($edge/4);$expected+=$set[1]*$blocks*$blocks*$block}
    if($bytes.Length -ne $expected){throw "DDS mip payload mismatch: $file"};++$count
}}
# Histogram resources use linear Gaussian values and an uncompressed inverse LUT.
foreach($special in @(@('gaussian',2048,2048,12,98),@('histogram',256,16,1,2))){
    $file=Join-Path $Cooked "environment_terrain_$($special[0]).dds"
    $bytes=[IO.File]::ReadAllBytes($file)
    if($bytes.Length -lt 148){throw "Truncated histogram DDS: $file"}
    foreach($check in @(@(0,0x20534444),@(4,124),@(12,$special[2]),@(16,$special[1]),@(28,$special[3]),@(76,32),@(84,0x30315844),@(128,$special[4]),@(132,3),@(140,4))){
        if([BitConverter]::ToUInt32($bytes,$check[0]) -ne $check[1]){throw "Histogram DDS header mismatch: $file"}
    }
    [long]$expected=148
    for($mip=0;$mip -lt $special[3];++$mip){
        $width=[Math]::Max(1,$special[1] -shr $mip);$height=[Math]::Max(1,$special[2] -shr $mip)
        if($special[4] -eq 2){$expected+=4*$width*$height*16}else{$expected+=4*[Math]::Ceiling($width/4)*[Math]::Ceiling($height/4)*16}
    }
    if($bytes.Length -ne $expected){throw "Histogram DDS payload mismatch: $file"}
    if($special[4] -eq 2){
        for($offset=148;$offset -lt $bytes.Length;$offset+=4){
            $value=[BitConverter]::ToSingle($bytes,$offset)
            if([float]::IsNaN($value) -or [float]::IsInfinity($value)){throw "Nonfinite histogram LUT: $file"}
        }
    }
    ++$count
}
$meshes=0
foreach($set in @(@('trunk',3),@('leaf',3),@('rock',4),@('grass',4))){for($variant=0;$variant -lt $set[1];++$variant){
    [long]$previous=3000001
    for($lod=0;$lod -lt 3;++$lod){
        $suffix=if($lod -eq 0){''}else{"_lod$lod"};$file=Join-Path $Cooked "environment_$($set[0])_$variant$suffix.meshbin"
        $bytes=[IO.File]::ReadAllBytes($file);if($bytes.Length -lt 16){throw "Truncated mesh: $file"}
        $vertices=[BitConverter]::ToUInt32($bytes,8)
        if([BitConverter]::ToUInt32($bytes,0) -ne 0x4d455348 -or [BitConverter]::ToUInt32($bytes,4) -ne 1 -or [BitConverter]::ToUInt32($bytes,12) -ne 76 -or $vertices -eq 0 -or $vertices%3 -ne 0 -or $bytes.Length -ne 16+[long]$vertices*76 -or $vertices -ge $previous){throw "Invalid LOD mesh payload: $file"}
        $previous=$vertices;++$meshes
    }
}}
Write-Output "environment.assets DDS=$count meshes=$meshes all payloads and LOD reductions valid"
