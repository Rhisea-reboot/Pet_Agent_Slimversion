# 精简回归测试：TTS 合成 + ASR 识别，任一失败即退出码非 0
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$py = Join-Path $root "runtime\Scripts\python.exe"

if (-not (Test-Path $py)) { $py = Join-Path $root "runtime\python.exe" }

if (-not (Test-Path $py)) { Write-Error "runtime python not found: $py"; exit 1 }

Write-Host "=== [1/2] TTS smoke ===" -ForegroundColor Cyan
& $py (Join-Path $PSScriptRoot "smoke_tts.py")
if ($LASTEXITCODE -ne 0) { Write-Error "TTS smoke FAILED"; exit 1 }

Write-Host "=== [2/2] ASR smoke ===" -ForegroundColor Cyan
& $py (Join-Path $PSScriptRoot "smoke_asr.py")
if ($LASTEXITCODE -ne 0) { Write-Error "ASR smoke FAILED"; exit 1 }

Write-Host "=== ALL SMOKE TESTS PASSED ===" -ForegroundColor Green
exit 0
