param(
    [string]$SourceDirectory = (Join-Path $PSScriptRoot "..\..\vendor\open-webSearch")
)

$ErrorActionPreference = "Stop"
$pidPath = Join-Path $SourceDirectory ".runtime\open-websearch.pid"

if (-not (Test-Path -LiteralPath $pidPath)) {
    Write-Host "open-webSearch is not running under this deployment script."
    exit 0
}

$processId = [int](Get-Content -LiteralPath $pidPath -Raw)
$process = Get-Process -Id $processId -ErrorAction SilentlyContinue
if ($null -ne $process) {
    Stop-Process -Id $processId -ErrorAction Stop
    Write-Host "Stopped open-webSearch process $processId."
}
else {
    Write-Host "Removed stale PID file for process $processId."
}

Remove-Item -LiteralPath $pidPath -Force
