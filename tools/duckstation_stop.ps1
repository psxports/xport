param([Parameter(Mandatory=$true)][string]$ProjectRoot,[string]$Role)
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $ProjectRoot).Path
$config=Get-Content -LiteralPath (Join-Path $root 'xport-project.json') -Raw | ConvertFrom-Json
if (-not $Role) {$Role=$config.duckstation.default_role}
$port=$config.duckstation.reserved_gdb_ports.$Role
if (-not $port) {throw 'Unreserved role'}
$file=Join-Path $root "status/runtime/duckstation-$port.json"
if (-not (Test-Path -LiteralPath $file)) {return}
$record=Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
$process=Get-Process -Id $record.pid -ErrorAction SilentlyContinue
if (-not $process) {return}
$expected=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'duckstation/distribution/duckstation-qt-x64-ReleaseLTCG.exe'))
$relative=$config.duckstation.data_directories.$Role
if (-not $relative) {$relative="tools/duckstation/data/$Role"}
$data=[IO.Path]::GetFullPath((Join-Path $root $relative))
if ($record.project -ne $root -or $record.data_directory -ne $data -or $record.port -ne $port -or $process.Path -ne $expected -or $record.executable -ne $expected -or $process.StartTime.ToFileTimeUtc() -ne $record.created) {throw 'Process or project identity differs; refusing to stop'}
Stop-Process -Id $process.Id
