param([Parameter(Mandatory=$true)][string]$ProjectRoot,[switch]$FastBoot,[string]$StateFile,[switch]$GameExe,[string]$Role)
$ErrorActionPreference='Stop'
$arguments=@('--project',$ProjectRoot,'duckstation_run')
if ($Role) {$arguments+=@('--role',$Role)}
if ($StateFile) {$arguments+=@('--state',$StateFile)}
if ($GameExe) {$arguments+='--game-exe'}
& python -B (Join-Path $PSScriptRoot 'xport.py') @arguments
if ($LASTEXITCODE -ne 0) {throw 'DuckStation launch failed'}
