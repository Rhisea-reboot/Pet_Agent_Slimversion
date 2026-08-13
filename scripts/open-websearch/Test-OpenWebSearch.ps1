param(
    [string]$BaseUrl = "http://127.0.0.1:3210",
    [switch]$IncludeSearch,
    [string]$SearchQuery = "open-webSearch health check",
    [string[]]$Engines = @("bing"),
    [ValidateSet("request", "auto", "playwright")]
    [string]$SearchMode = "request"
)

$ErrorActionPreference = "Stop"
$health = Invoke-RestMethod -Method Get -Uri "$BaseUrl/health" -TimeoutSec 5
$status = Invoke-RestMethod -Method Get -Uri "$BaseUrl/status" -TimeoutSec 5

if ($health.status -ne "ok" -or $health.data.daemon -ne "running") {
    throw "Health endpoint returned an unexpected payload."
}
if ($status.status -ne "ok" -or $status.data.daemon -ne "running") {
    throw "Status endpoint returned an unexpected payload."
}

Write-Host "Health: OK"
Write-Host "Base URL: $($status.data.baseUrl)"
Write-Host "Allowed engines: $($status.data.configSummary.allowedSearchEngines -join ', ')"
Write-Host "CORS: disabled by the local start script; the REST daemon has no built-in authentication."

if ($IncludeSearch) {
    $payload = @{ query = $SearchQuery; limit = 1; engines = $Engines; searchMode = $SearchMode } | ConvertTo-Json -Compress
    try {
        $result = Invoke-RestMethod -Method Post -Uri "$BaseUrl/search" -ContentType "application/json" -Body $payload -TimeoutSec 15
    }
    catch {
        throw "Search verification did not finish within the 15-second interactive budget or returned a transport error: $($_.Exception.Message)"
    }
    if ($result.status -ne "ok") {
        throw "Search endpoint returned status '$($result.status)'."
    }
    Write-Host "Search: OK ($($result.data.totalResults) result(s), engines: $($Engines -join ', '))"
}
