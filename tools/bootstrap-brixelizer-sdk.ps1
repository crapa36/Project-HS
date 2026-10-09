param([switch]$Offline,[switch]$PrintPath)
$ErrorActionPreference='Stop'
$brixProject=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$brixCache=Join-Path $brixProject 'Build/tooling/render-sdks/brixelizer-1.1.4'
$brixCommit='c6efa6bf7f2027b3ec94f28578bb5965eabb9e55'
$brixSource=Join-Path $brixCache "FidelityFX-SDK-$brixCommit"
$brixArchive=Join-Path $brixCache 'sdk-source.zip'
$brixManifest=Join-Path $brixCache 'source-files.sha256'
$brixArchiveHash='b5bba71656e18f4c813fda4eb364dcb5e9b8491efd6abb66fdc8a2b530bfcedd'
$brixManifestHash='1f5bf0cb5a2541626c66546b0a22ac410c747fb6273c65bf3f8c6d54e9bb2761'
function Assert-BrixHash([string]$Path,[string]$Expected) {
    if(!(Test-Path -LiteralPath $Path -PathType Leaf) -or
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Expected) {
        throw "Pinned Brixelizer input is missing or modified: $Path"
    }
}
if(!(Test-Path -LiteralPath $brixSource)) {
    if($Offline) { throw "Pinned Brixelizer SDK is absent. Run Tools/bootstrap-brixelizer-sdk.ps1." }
    New-Item -ItemType Directory -Force -Path $brixCache | Out-Null
    if(!(Test-Path -LiteralPath $brixArchive)) {
        Invoke-WebRequest -Uri "https://codeload.github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/zip/$brixCommit" -OutFile $brixArchive
    }
    Assert-BrixHash $brixArchive $brixArchiveHash
    Expand-Archive -LiteralPath $brixArchive -DestinationPath $brixCache
}
if(!(Test-Path -LiteralPath $brixManifest)) {
    # This deterministic list is tied to the verified archive by its fixed hash.
    Assert-BrixHash $brixArchive $brixArchiveHash
    $brixEntries=@(Get-ChildItem (Join-Path $brixSource 'sdk') -Recurse -File | ForEach-Object {
        $brixRelative=[IO.Path]::GetRelativePath($brixSource,$_.FullName).Replace('\','/')
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),$brixRelative
    } | Sort-Object -CaseSensitive)
    $brixEntries+=('{0}  LICENSE.txt' -f (Get-FileHash -LiteralPath (Join-Path $brixSource 'LICENSE.txt')).Hash.ToLowerInvariant())
    [IO.File]::WriteAllText($brixManifest,($brixEntries -join "`n")+"`n",[Text.UTF8Encoding]::new($false))
}
Assert-BrixHash $brixManifest $brixManifestHash
foreach($brixLine in [IO.File]::ReadAllLines($brixManifest)) {
    if($brixLine -cnotmatch '^([0-9a-f]{64})  (sdk/[^\r\n]+|LICENSE\.txt)$') { throw 'Invalid pinned Brixelizer source manifest.' }
    $brixDigest=$Matches[1];$brixRelative=$Matches[2]
    $brixFile=[IO.Path]::GetFullPath((Join-Path $brixSource $brixRelative))
    if(!$brixFile.StartsWith($brixSource+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Brixelizer source path escaped its cache.' }
    Assert-BrixHash $brixFile $brixDigest
}
if($PrintPath) { Write-Output $brixSource }
else { Write-Output "Verified FidelityFX SDK1.1.4/Brixelizer1.0.0/GI1.0.1: $brixSource" }
