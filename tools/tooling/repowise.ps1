[CmdletBinding()]
param(
    [ValidateSet('context','symbol')][string]$Action = 'context',
    [Parameter(Mandatory=$true)][string]$Query
)
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$tool=Join-Path $repo 'Build/tooling/repowise/venv/Scripts/repowise.exe'
if (!(Test-Path $tool)) { throw 'Install the qualified Repowise version first; see WORKFLOW.md.' }
$env:DO_NOT_TRACK='1'
$env:REPOWISE_TELEMETRY_DISABLED='1'
$env:REPOWISE_SKIP_EDITOR_SETUP='1'
$env:REPOWISE_NO_SAVE_KEY='1'
& $tool $Action $Query --path $repo --no-workspace --format=json
exit $LASTEXITCODE
