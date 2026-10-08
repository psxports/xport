param(
    [Parameter(Mandatory = $true)][string]$Project,
    [Parameter(Mandatory = $true)][ValidateSet('start', 'stop', 'status')][string]$Action,
    [ValidatePattern('^[A-Za-z0-9_-]{1,40}$')][string]$Name,
    [string]$State,
    [ValidateRange(1, 10)][int]$Slot,
    [ValidateRange(1, 60)][int]$Timeout = 30
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath $Project).Path
$launcher = Join-Path $PSScriptRoot 'xport.py'
$arguments = @('-B', $launcher, '--project', $projectRoot, 'interactive_record', $Action)
if ($Action -eq 'start') {
    if (-not $Name) {
        throw 'Name is required for start'
    }
    if ((-not $State) -eq (-not $PSBoundParameters.ContainsKey('Slot'))) {
        throw 'Choose exactly one State or Slot for start'
    }
    $arguments += $Name
    if ($State) {
        $statePath = if ([IO.Path]::IsPathRooted($State)) { $State } else { Join-Path $projectRoot $State }
        $statePath = (Resolve-Path -LiteralPath $statePath).Path
        $arguments += @('--state', $statePath)
    } else {
        $arguments += @('--slot', [string]$Slot)
    }
} elseif ($Action -eq 'stop') {
    if ($Name) {
        throw 'Name is not accepted for stop'
    }
} else {
    if (-not $Name) {
        throw 'Name is required for status'
    }
    $arguments += $Name
}
$arguments += @('--timeout', [string]$Timeout, '--wait', [string]$Timeout)
& python @arguments
exit $LASTEXITCODE
