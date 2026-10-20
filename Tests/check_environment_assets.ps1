param([Parameter(Mandatory=$true)][string]$GenerationPointer)
$ErrorActionPreference='Stop'

$pointerPath = [IO.Path]::GetFullPath($GenerationPointer)
if (!(Test-Path -LiteralPath $pointerPath -PathType Leaf)) {
    throw "Generation pointer does not exist: $pointerPath"
}
$pointer = Get-Content -LiteralPath $pointerPath -Raw | ConvertFrom-Json
if ($pointer.schema_version -ne 1 -or
    [string]::IsNullOrWhiteSpace([string]$pointer.generation_id) -or
    [string]::IsNullOrWhiteSpace([string]$pointer.manifest_locator)) {
    throw "Invalid Generation pointer: $pointerPath"
}
$buildRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path $pointerPath -Parent) '..'))
$manifestPath = [IO.Path]::GetFullPath((Join-Path $buildRoot ([string]$pointer.manifest_locator)))
if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Generation manifest does not exist: $manifestPath"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema_version -notin @(2, 3) -or
    [string]$manifest.generation_id -ne [string]$pointer.generation_id -or
    $null -eq $manifest.products) {
    throw "Generation manifest does not match pointer: $manifestPath"
}
$generationProducts = @{}
foreach ($product in $manifest.products) {
    $logicalId = [string]$product.logical_id
    $locator = [string]$product.locator
    if ([string]::IsNullOrWhiteSpace($logicalId) -or
        [string]::IsNullOrWhiteSpace($locator) -or
        $generationProducts.ContainsKey($logicalId)) {
        throw "Invalid or duplicate Generation product in: $manifestPath"
    }
    $productPath = [IO.Path]::GetFullPath((Join-Path $buildRoot $locator))
    if (!(Test-Path -LiteralPath $productPath -PathType Leaf)) {
        throw "Generation product path does not exist for $logicalId`: $productPath"
    }
    $generationProducts[$logicalId] = $productPath
}
function Resolve-EnvironmentProduct {
    param([Parameter(Mandatory=$true)][string]$FileName)
    $logicalId = "environment/$FileName"
    if (!$generationProducts.ContainsKey($logicalId)) {
        throw "Generation is missing required environment product: $logicalId"
    }
    return $generationProducts[$logicalId]
}
function Read-FilePrefix {
    param([Parameter(Mandatory=$true)][string]$Path,[Parameter(Mandatory=$true)][int]$Count)
    $stream=[IO.File]::OpenRead($Path)
    try {
        $bytes=New-Object byte[] $Count
        $read=0
        while($read -lt $Count){
            $chunk=$stream.Read($bytes,$read,$Count-$read)
            if($chunk -le 0){break}
            $read+=$chunk
        }
        [pscustomobject]@{Bytes=$bytes;BytesRead=$read;Length=$stream.Length}
    } finally {
        $stream.Dispose()
    }
}
function Assert-FiniteFloatPayload {
    param([Parameter(Mandatory=$true)][string]$Path,[Parameter(Mandatory=$true)][long]$Offset,[Parameter(Mandatory=$true)][long]$Length)
    $stream=[IO.File]::OpenRead($Path)
    try {
        $stream.Position=$Offset
        $remaining=$Length
        $buffer=New-Object byte[] (64*1024)
        while($remaining -gt 0){
            $requested=if($remaining -gt $buffer.Length){$buffer.Length}else{[int]$remaining}
            $read=0
            while($read -lt $requested){
                $chunk=$stream.Read($buffer,$read,$requested-$read)
                if($chunk -le 0){throw "Truncated histogram LUT: $Path"}
                $read+=$chunk
            }
            for($offset=0;$offset -lt $requested;$offset+=4){
                $value=[BitConverter]::ToSingle($buffer,$offset)
                if([float]::IsNaN($value) -or [float]::IsInfinity($value)){throw "Nonfinite histogram LUT: $Path"}
            }
            $remaining-=$requested
        }
    } finally {
        $stream.Dispose()
    }
}
$sets=@(@('terrain',4,12,@('basecolor','surface','normal','detail','height')),@('rock',4,12,@('basecolor','surface','normal','detail')),@('bark',3,12,@('basecolor','surface','normal','detail')),@('grass',2,6,@('basecolor','normal','roughness')),@('leaf',3,6,@('basecolor','normal','roughness')),@('ground_detail',2,12,@('basecolor','surface','normal')))
$renderTextureNames=@('terrain_gaussian','terrain_surface','terrain_normal','terrain_detail','terrain_height',
    'rock_basecolor','rock_surface','rock_normal','rock_detail',
    'bark_basecolor','bark_surface','bark_normal','bark_detail',
    'grass_basecolor','grass_normal','grass_roughness',
    'leaf_basecolor','leaf_normal','leaf_roughness',
    'ground_detail_basecolor','ground_detail_surface','ground_detail_normal')
foreach($name in $renderTextureNames){
    $logicalId="render/texture/environment_$name.rpkg"
    if(!$generationProducts.ContainsKey($logicalId)){
        throw "Generation is missing required render texture product: $logicalId"
    }
}
$count=0
foreach($set in $sets){ foreach($role in $set[3]){
    $file=Resolve-EnvironmentProduct "environment_$($set[0])_$role.dds"
    $asset=Read-FilePrefix $file 148
    $bytes=$asset.Bytes
    if($asset.Length -lt 148 -or $asset.BytesRead -lt 148){throw "Truncated DDS: $file"}
    $format=if($role -eq 'basecolor'){99}elseif($role -in @('height','roughness')){80}else{83}
    foreach($check in @(@(0,0x20534444),@(4,124),@(12,2048),@(16,2048),@(28,$set[2]),@(76,32),@(84,0x30315844),@(128,$format),@(132,3),@(140,$set[1]))){
        if([BitConverter]::ToUInt32($bytes,$check[0]) -ne $check[1]){throw "DDS header mismatch at $($check[0]): $file"}
    }
    $block=if($format -eq 80){8}else{16};[long]$expected=148
    for($mip=0;$mip -lt $set[2];++$mip){$edge=[Math]::Max(1,2048 -shr $mip);$blocks=[Math]::Ceiling($edge/4);$expected+=$set[1]*$blocks*$blocks*$block}
    if($asset.Length -ne $expected){throw "DDS mip payload mismatch: $file"};++$count
}}
# Histogram resources use linear Gaussian values and an uncompressed inverse LUT.
foreach($special in @(@('gaussian',2048,2048,12,98),@('histogram',256,16,1,2))){
    $file=Resolve-EnvironmentProduct "environment_terrain_$($special[0]).dds"
    $asset=Read-FilePrefix $file 148
    $bytes=$asset.Bytes
    if($asset.Length -lt 148 -or $asset.BytesRead -lt 148){throw "Truncated histogram DDS: $file"}
    foreach($check in @(@(0,0x20534444),@(4,124),@(12,$special[2]),@(16,$special[1]),@(28,$special[3]),@(76,32),@(84,0x30315844),@(128,$special[4]),@(132,3),@(140,4))){
        if([BitConverter]::ToUInt32($bytes,$check[0]) -ne $check[1]){throw "Histogram DDS header mismatch: $file"}
    }
    [long]$expected=148
    for($mip=0;$mip -lt $special[3];++$mip){
        $width=[Math]::Max(1,$special[1] -shr $mip);$height=[Math]::Max(1,$special[2] -shr $mip)
        if($special[4] -eq 2){$expected+=4*$width*$height*16}else{$expected+=4*[Math]::Ceiling($width/4)*[Math]::Ceiling($height/4)*16}
    }
    if($asset.Length -ne $expected){throw "Histogram DDS payload mismatch: $file"}
    if($special[4] -eq 2){
        Assert-FiniteFloatPayload $file 148 ($asset.Length-148)
    }
    ++$count
}
$meshes=0
$meshNames=@()
foreach($set in @(@('trunk',3),@('leaf',3),@('rock',4),@('grass',4))){for($variant=0;$variant -lt $set[1];++$variant){$meshNames+="$($set[0])_$variant"}}
$meshNames+=@('flower_daisy','flower_buttercup','flower_violet','clover_patch','ground_leaves','twig_cluster','fence_short','shrub_low','stump_roots','crate_intact','crate_broken','barrel_intact','barrel_broken','cart_broken','fence_broken','fence_splitrail','sack_tied','fallen_log','rock_mossy')
foreach($name in $meshNames){
    [long]$previous=3000001
    for($lod=0;$lod -lt 3;++$lod){
        $suffix=if($lod -eq 0){''}else{"_lod$lod"};$file=Resolve-EnvironmentProduct "environment_$name$suffix.meshbin"
        $asset=Read-FilePrefix $file 16;$bytes=$asset.Bytes;if($asset.Length -lt 16 -or $asset.BytesRead -lt 16){throw "Truncated mesh: $file"}
        $vertices=[BitConverter]::ToUInt32($bytes,8)
        if([BitConverter]::ToUInt32($bytes,0) -ne 0x4d455348 -or [BitConverter]::ToUInt32($bytes,4) -ne 1 -or [BitConverter]::ToUInt32($bytes,12) -ne 76 -or $vertices -eq 0 -or $vertices%3 -ne 0 -or $asset.Length -ne 16+[long]$vertices*76 -or $vertices -ge $previous){throw "Invalid LOD mesh payload: $file"}
        $previous=$vertices;++$meshes
    }
}
Write-Output "environment.assets DDS=$count render-texture-packages=$($renderTextureNames.Count) meshes=$meshes all payloads and LOD reductions valid"
