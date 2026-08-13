# =============================================================================
# TTS 冲击测试（Phase 4 验收项）
#   - 连续会话指定时长内反复合成，验证 Kokoro 常驻进程内存无异常增长
#   - 结束时按 C++ TtsServerManager::Stop() 的方式终止进程，验证端口被回收
#
# 用法:
#   .\scripts\stress_tts.ps1                                  # 默认 30 分钟
#   .\scripts\stress_tts.ps1 -DurationMinutes 2 -TolerancePercent 25
#
# 退出码: 0 = 通过；1 = 失败（内存增长超限 / 请求失败过多 / 进程回收失败）
# =============================================================================
[CmdletBinding()]
param(
    [int]$DurationMinutes = 30,
    [int]$TolerancePercent = 20,
    [int]$MaxRequestFailures = 3,
    [string]$ServerUrl = "http://127.0.0.1:9880",
    [string]$LogDirectory = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($LogDirectory)) {
    $LogDirectory = Join-Path $env:TEMP "vpet_stress"
}
New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null

$python = Join-Path $root "runtime\Scripts\python.exe"
if (-not (Test-Path -LiteralPath $python)) { $python = Join-Path $root "runtime\python.exe" }
if (-not (Test-Path -LiteralPath $python)) { throw "runtime python not found: $python" }

$sentences = @(
    "你好，今天过得怎么样？",
    "我要出门散步了，很快回来。",
    "记得多喝水，保持好心情。",
    "今天的天气真不错。",
    "我给你讲个有趣的事情吧。",
    "工作辛苦了，休息一下吧。",
    "晚安，做个好梦。",
    "Hello, how are you today?",
    "周末有什么安排吗？",
    "祝你一切顺利。"
)

# ---- 1. 前置检查：目标端口不得已被占用 ----
$healthUrl = "$ServerUrl/health"
$portOccupied = $false
try {
    $null = Invoke-RestMethod -Uri $healthUrl -TimeoutSec 3
    $portOccupied = $true
}
catch [System.Net.WebException] {
    $portOccupied = $false
}
catch {
    $portOccupied = $false
}
if ($portOccupied) {
    throw "A server is already listening on $ServerUrl; stop it before running the stress test."
}

# ---- 2. 启动 kokoro_server.py 常驻进程（与 TtsServerManager 同一启动方式） ----
$stderrLog = Join-Path $LogDirectory "kokoro_server.stderr.log"
$serverPort = ([uri]$ServerUrl).Port
$serverProcess = Start-Process -FilePath $python `
    -ArgumentList @((Join-Path $root "tools\kokoro\kokoro_server.py"), "--host", "127.0.0.1", "--port", "$serverPort") `
    -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardError $stderrLog

try {
    $ready = $false
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        Start-Sleep -Seconds 1
        if ($serverProcess.HasExited) { break }
        try {
            $health = Invoke-RestMethod -Uri $healthUrl -TimeoutSec 2
            if ($health.status -eq "ok") { $ready = $true; break }
        }
        catch { }
    }
    if (-not $ready) {
        throw "Kokoro server did not become ready within 60s (see $stderrLog)"
    }
    Write-Host "Kokoro server ready (pid $($serverProcess.Id))."

    # ---- 3. 预热（加载 KPipeline），以预热后的内存作为基线 ----
    $outWav = Join-Path $LogDirectory "stress_last.wav"
    foreach ($text in $sentences[0..2]) {
        Invoke-RestMethod -Uri "$ServerUrl/tts" -Method Post `
            -ContentType "application/json" `
            -Body (@{ text = $text; lang = "z"; voice = "zf_xiaobei"; speed = 1.0 } | ConvertTo-Json -Compress) `
            -OutFile $outWav -TimeoutSec 120 | Out-Null
    }

    $deadline = [datetime]::Now.AddMinutes($DurationMinutes)
    $requestFailures = 0
    $iteration = 0
    $results = [System.Collections.Generic.List[object]]::new()

    # ---- 4. 循环合成 + 内存采样 ----
    while ([datetime]::Now -lt $deadline) {
        $iteration++
        $text = $sentences[$iteration % $sentences.Count]
        $ok = $true
        try {
            Invoke-RestMethod -Uri "$ServerUrl/tts" -Method Post `
                -ContentType "application/json" `
                -Body (@{ text = $text; lang = "z"; voice = "zf_xiaobei"; speed = 1.0 } | ConvertTo-Json -Compress) `
                -OutFile $outWav -TimeoutSec 120 | Out-Null
        }
        catch {
            $requestFailures++
            $ok = $false
            Write-Warning "Synthesis request #$iteration failed: $($_.Exception.Message)"
            if ($requestFailures -ge $MaxRequestFailures) { throw "Too many synthesis failures." }
        }

        $sampled = Get-Process -Id $serverProcess.Id -ErrorAction SilentlyContinue
        if ($null -eq $sampled) { throw "Kokoro server process exited during the stress test." }
        $workingSetMb = [math]::Round($sampled.WorkingSet64 / 1MB, 2)
        $privateMb = [math]::Round($sampled.PrivateMemorySize64 / 1MB, 2)

        $results.Add([PSCustomObject]@{
            Iteration    = $iteration
            Timestamp    = (Get-Date).ToString("HH:mm:ss")
            Status       = if ($ok) { "ok" } else { "failed" }
            WorkingSetMB = $workingSetMb
            PrivateMB    = $privateMb
        })
    }

    # ---- 5. 汇总与判定 ----
    $summary = $results | ConvertTo-Csv -NoTypeInformation
    $results | ConvertTo-Csv -NoTypeInformation | Out-File (Join-Path $LogDirectory "tts_stress.csv") -Encoding utf8

    $baseline = $results[0].WorkingSetMB
    $latest = $results[$results.Count - 1].WorkingSetMB
    $peak = ($results | Measure-Object -Property WorkingSetMB -Maximum).Maximum
    $growthMb = $latest - $baseline
    $limitMb = [math]::Round($baseline * ($TolerancePercent / 100.0) + 100, 2)

    Write-Host "=== TTS stress summary ==="
    Write-Host ("Iterations: {0} | Duration: {1} min | Failures: {2}" -f $iteration, $DurationMinutes, $requestFailures)
    Write-Host ("WorkingSet MB: baseline {0} | peak {1} | latest {2} | growth {3}" -f $baseline, $peak, $latest, $growthMb)
    Write-Host ("Growth limit: {0} MB (tolerance {1}% + 100 MB)" -f $limitMb, $TolerancePercent)

    if ($requestFailures -ge $MaxRequestFailures) { throw "Too many synthesis failures ($requestFailures)." }
    if ($growthMb -gt $limitMb) { throw "Memory grew $growthMb MB, above the $limitMb MB limit." }

    # ---- 6. 进程回收：模拟 TtsServerManager::Stop() 终止 + 端口释放校验 ----
    $serverProcess | Stop-Process -Force
    $serverProcess.WaitForExit()
    Write-Host "Kokoro server terminated (exit code $($serverProcess.ExitCode)); waiting for port release..."

    Start-Sleep -Seconds 2
    $tcpClient = [System.Net.Sockets.TcpClient]::new()
    $released = $true
    try {
        $tcpClient.Connect("127.0.0.1", $serverPort)
        $released = $false
    }
    catch {
        $released = $true
    }
    finally {
        $tcpClient.Dispose()
    }
    if (-not $released) { throw "Port $serverPort is still occupied after server termination." }

    Write-Host "Port $serverPort released. Stress test PASSED." -ForegroundColor Green
    exit 0
}
finally {
    if ($null -ne $serverProcess -and -not $serverProcess.HasExited) {
        $serverProcess | Stop-Process -Force
        $serverProcess.WaitForExit()
    }
}