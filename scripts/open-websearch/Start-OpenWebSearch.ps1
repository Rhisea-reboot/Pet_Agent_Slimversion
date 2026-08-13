param(
    [string]$SourceDirectory = (Join-Path $PSScriptRoot "..\..\vendor\open-webSearch"),
    [string]$HostAddress = "127.0.0.1",
    [int]$Port = 3210,
    [string]$AllowedSearchEngines = "bing",
    [ValidateSet("bing", "duckduckgo", "startpage", "sogou")]
    [string]$DefaultSearchEngine = "bing",
    [ValidateSet("request", "auto", "playwright")]
    [string]$SearchMode = "request",
    [switch]$UseProxy,
    [string]$ProxyUrl = "http://127.0.0.1:7890"
)

$ErrorActionPreference = "Stop"

if ($HostAddress -ne "127.0.0.1" -and $HostAddress -ne "::1") {
    throw "This script only permits loopback binding. Use a reverse proxy with authentication for remote access."
}

$entryPoint = Join-Path $SourceDirectory "build\index.js"
if (-not (Test-Path -LiteralPath $entryPoint)) {
    throw "Build output is missing. Run Install-OpenWebSearch.ps1 first."
}

$runtimeDirectory = Join-Path $SourceDirectory ".runtime"
$pidPath = Join-Path $runtimeDirectory "open-websearch.pid"
$stdoutPath = Join-Path $runtimeDirectory "open-websearch.stdout.log"
$stderrPath = Join-Path $runtimeDirectory "open-websearch.stderr.log"
New-Item -ItemType Directory -Path $runtimeDirectory -Force | Out-Null

if (Test-Path -LiteralPath $pidPath) {
    $existingPid = [int](Get-Content -LiteralPath $pidPath -Raw)
    if (Get-Process -Id $existingPid -ErrorAction SilentlyContinue) {
        throw "open-webSearch is already running with PID $existingPid."
    }
    Remove-Item -LiteralPath $pidPath -Force
}

$listener = New-Object System.Net.Sockets.TcpClient
try {
    $listener.Connect($HostAddress, $Port)
    throw "Port $Port on $HostAddress is already in use."
}
catch [System.Net.Sockets.SocketException] {
    # A refused connection means no listener owns the requested loopback port.
}
finally {
    $listener.Dispose()
}

$node = (Get-Command node -ErrorAction Stop).Source
$previousDefaultEngine = $env:DEFAULT_SEARCH_ENGINE
$previousAllowedEngines = $env:ALLOWED_SEARCH_ENGINES
$previousSearchMode = $env:SEARCH_MODE
$previousUseProxy = $env:USE_PROXY
$previousProxyUrl = $env:PROXY_URL
$previousEnableCors = $env:ENABLE_CORS
$env:DEFAULT_SEARCH_ENGINE = $DefaultSearchEngine
$env:ALLOWED_SEARCH_ENGINES = $AllowedSearchEngines
$env:SEARCH_MODE = $SearchMode
$env:USE_PROXY = if ($UseProxy) { "true" } else { "false" }
$env:PROXY_URL = $ProxyUrl
$env:ENABLE_CORS = "false"
try {
    $process = Start-Process -FilePath $node `
        -ArgumentList @("build/index.js", "serve", "--host", $HostAddress, "--port", $Port) `
        -WorkingDirectory $SourceDirectory `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -PassThru
}
finally {
    $env:DEFAULT_SEARCH_ENGINE = $previousDefaultEngine
    $env:ALLOWED_SEARCH_ENGINES = $previousAllowedEngines
$env:SEARCH_MODE = $previousSearchMode
$env:USE_PROXY = $previousUseProxy
$env:PROXY_URL = $previousProxyUrl
    $env:ENABLE_CORS = $previousEnableCors
}

$process.Id | Set-Content -LiteralPath $pidPath -NoNewline

Start-Sleep -Milliseconds 500
if ($process.HasExited) {
    Remove-Item -LiteralPath $pidPath -Force -ErrorAction SilentlyContinue
    throw "open-webSearch exited during startup. Check $stderrPath."
}

Write-Host "open-webSearch started with PID $($process.Id) at http://$HostAddress`:$Port"
Write-Host "CORS is disabled. The service is only bound to loopback."
