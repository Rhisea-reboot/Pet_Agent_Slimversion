# 下载 BGE 中文 embedding 模型（ONNX 格式，源: hf-mirror 镜像的 Xenova 仓库）
#
# 部署目录: models\embedding\bge-small-zh-v1.5\
# 之后需在 memory_config.json 中配置 embedding 的 model_dir。
#
# 用法:  powershell -ExecutionPolicy Bypass -File scripts\download_bge_model.ps1

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$ModelDir = Join-Path $Root "models\embedding\bge-small-zh-v1.5"
$BaseUrl = "https://hf-mirror.com/Xenova/bge-small-zh-v1.5/resolve/main"

$Files = @(
    "onnx/model.onnx",
    "onnx/model_quantized.onnx",
    "tokenizer.json",
    "tokenizer_config.json",
    "vocab.txt",
    "config.json"
)

Write-Host "目标目录: $ModelDir"
New-Item -ItemType Directory -Force -Path $ModelDir | Out-Null

foreach ($File in $Files) {
    $Dest = Join-Path $ModelDir $File
    New-Item -ItemType Directory -Force -Path (Split-Path $Dest) | Out-Null
    if (Test-Path $Dest) {
        Write-Host "跳过（已存在）: $File"
        continue
    }
    $Url = "$BaseUrl/$File"
    Write-Host "下载: $Url"
    try {
        Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
    } catch {
        Write-Host "下载失败: $File  -> $($_.Exception.Message)"
        if (Test-Path $Dest) { Remove-Item $Dest -Force }
    }
}

Write-Host ""
Write-Host "完成。请在 memory_config.json 中设置:"
Write-Host '  "embedding": { "backend": "local_onnx", "enabled": true, "model_dir": "models/embedding/bge-small-zh-v1.5", "onnx_model": "onnx/model.onnx", "tokenizer": "tokenizer.json" }'
