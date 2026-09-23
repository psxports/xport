param(
    [Parameter(Mandatory=$true)][string]$SessionPath,
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [int]$Seconds = 610
)
$ErrorActionPreference = 'Stop'
$session = Get-Content -LiteralPath $SessionPath -Raw | ConvertFrom-Json
$process = Get-Process -Id $session.process_identity.pid
$expected = $session.executable
if (-not $expected) {$expected = Join-Path $session.runtime 'duckstation-qt-x64-ReleaseLTCG.exe'}
if ($process.Path -ne $expected -or $process.StartTime.ToFileTimeUtc() -ne $session.process_identity.created) {
    throw 'Recorder identity changed'
}
$stream = [System.IO.File]::Open($OutputPath, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
$writer = [System.IO.StreamWriter]::new($stream)
$watch = [System.Diagnostics.Stopwatch]::StartNew()
try {
    while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
        $process.Refresh()
        if ($process.HasExited) { break }
        $row = [ordered]@{
            elapsed_seconds = $watch.Elapsed.TotalSeconds
            utc = [DateTime]::UtcNow.ToString('o')
            cpu_seconds = $process.TotalProcessorTime.TotalSeconds
            working_set_bytes = $process.WorkingSet64
            private_bytes = $process.PrivateMemorySize64
            lifetime_peak_working_set_bytes = $process.PeakWorkingSet64
        }
        $writer.WriteLine(($row | ConvertTo-Json -Compress))
        $writer.Flush()
        Start-Sleep -Milliseconds 1000
    }
} finally {
    $writer.Dispose()
    $process.Dispose()
}
