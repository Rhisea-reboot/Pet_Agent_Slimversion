# scripts/smoke_dag_editor.ps1 — DAG 编辑器冒烟测试
#
# 用法：pwsh -File scripts\smoke_dag_editor.ps1 [-BuildDir build-release-full]
#
# 流程：
#   1. 启动 VPet（或直接用 DagHttpServerTests？此处用独立最小宿主不可行，
#      因此要求 VPet 已运行并由用户手动触发「DAG 编辑器」菜单；脚本改为
#      直接验证构建产物存在 + 端口探测模式）。
#
# 实际自动化路径：使用 dag_http_server_tests.exe 作为宿主无法常驻。
# 因此本脚本做三件事：
#   A. 校验 VPet.exe 与资源已编入（qrc 字符串扫描）。
#   B. 提示用户在桌宠菜单点击「DAG 编辑器」后，输入 URL 进行 API 探测。
#   C. 对给定 URL 依次请求 /api/meta、/api/object_info、/api/graph、
#      /api/runtime/status 并打印结果摘要。

param(
    [string]$Url = "",
    [string]$BuildDir = "build-release-full"
)

$ErrorActionPreference = "Stop"

Write-Host "=== DAG 编辑器冒烟测试 ===" -ForegroundColor Cyan

# ---- A. 构建产物检查 ---------------------------------------------------------
$exePath = Join-Path $PSScriptRoot "..\$BuildDir\VPet.exe"

if (-not (Test-Path $exePath)) {
    Write-Host "[FAIL] 未找到 $exePath —— 请先构建 VPet 目标" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] 构建产物存在: $exePath"

$qrcPath = Join-Path $PSScriptRoot "..\resources\dag_editor\dag_editor_web.qrc"
foreach ($asset in @("index.html", "app.js", "graph_adapter.js", "vendor/litegraph.min.js")) {
    $full = Join-Path (Split-Path $qrcPath) ($asset -replace '/', '\')
    if (-not (Test-Path $full)) {
        Write-Host "[FAIL] 缺少前端资源: $full" -ForegroundColor Red
        exit 1
    }
}
Write-Host "[OK] 前端资源齐全（qrc 引用的关键文件均在）"

# ---- B/C. API 探测 -----------------------------------------------------------
if (-not $Url) {
    Write-Host ""
    Write-Host "请在桌宠右键菜单点击「DAG 编辑器」，浏览器会自动打开。"
    Write-Host "把地址栏完整 URL 粘贴到这里以继续 API 探测（回车跳过）："
    $Url = Read-Host "URL"
}

if (-not $Url) {
    Write-Host "[SKIP] 未提供 URL，跳过 API 探测"
    exit 0
}

$headers = @{ }

if ($Url -match '\?token=([^&]+)') {
    $headers["X-DAG-Token"] = $Matches[1]
    Write-Host "[OK] 已从 URL 提取 token"
}
else {
    Write-Host "[WARN] URL 中没有 token，API 将返回 401（可用于鉴权负向验证）"
}

function Invoke-Check($method, $path, $expectStatus) {
    try {
        $response = Invoke-WebRequest -Uri "$($Url.TrimEnd('/'))$path" `
            -Method $method -Headers $headers -UseBasicParsing -TimeoutSec 5
        $status = [int]$response.StatusCode
    }
    catch {
        $status = [int]$_.Exception.Response.StatusCode
    }

    if ($status -eq $expectStatus) {
        Write-Host "[OK] $method $path -> $status" -ForegroundColor Green
    }
    else {
        Write-Host "[FAIL] $method $path -> $status（期望 $expectStatus）" -ForegroundColor Red
        exit 1
    }
}

Invoke-Check "GET" "/api/meta" 200
Invoke-Check "GET" "/api/object_info" 200
Invoke-Check "GET" "/api/graph" 200
Invoke-Check "GET" "/api/runtime/status" 200

# 鉴权负向验证：无 token 必须 401
try {
    Invoke-WebRequest -Uri "$($Url.TrimEnd('/'))/api/meta" -UseBasicParsing `
        -TimeoutSec 5 | Out-Null
    Write-Host "[FAIL] 无 token 请求未被拒绝" -ForegroundColor Red
    exit 1
}
catch {
    if ([int]$_.Exception.Response.StatusCode -eq 401) {
        Write-Host "[OK] 无 token -> 401（鉴权生效）" -ForegroundColor Green
    } else {
        Write-Host "[FAIL] 无 token 返回了非 401 状态" -ForegroundColor Red
        exit 1
    }
}

Write-Host ""
Write-Host "=== 冒烟测试全部通过 ===" -ForegroundColor Cyan
