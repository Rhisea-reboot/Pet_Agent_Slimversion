# 下载并部署 onnxruntime 官方 Windows 运行时（MSVC ABI 构建）
#
# VPet 通过动态加载其纯 C 入口（OrtGetApiBase）使用推理能力，不链接
# MSVC 导入库，因此 MinGW 工具链亦可运行。
#
# 用法:  powershell -ExecutionPolicy Bypass -File scripts\download_onnxruntime.ps1 [-Version 1.28.0]

param(
    [string]$Version = "1.28.0"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$VendorDir = Join-Path $Root "vendor\onnxruntime"
$ZipName = "onnxruntime-win-x64-$Version.zip"
$ZipPath = Join-Path $env:TEMP $ZipName
$Url = "https://github.com/microsoft/onnxruntime/releases/download/v$Version/$ZipName"

Write-Host "[1/3] 下载 $Url"
if (-not (Test-Path $ZipPath)) {
    Invoke-WebRequest -Uri $Url -OutFile $ZipPath -UseBasicParsing
} else {
    Write-Host "      已存在缓存: $ZipPath"
}

Write-Host "[2/3] 解压"
$ExtractDir = Join-Path $env:TEMP "onnxruntime-$Version-extract"
if (Test-Path $ExtractDir) {
    Remove-Item -Recurse -Force $ExtractDir
}
Expand-Archive -Path $ZipPath -DestinationPath $ExtractDir -Force
$PkgDir = Join-Path $ExtractDir "onnxruntime-win-x64-$Version"

Write-Host "[3/3] 部署到 vendor\onnxruntime"
New-Item -ItemType Directory -Force -Path (Join-Path $VendorDir "include\onnxruntime") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $VendorDir "bin") | Out-Null

$Headers = @(
    "onnxruntime_c_api.h",
    "onnxruntime_cxx_api.h",
    "onnxruntime_cxx_inline.h",
    "onnxruntime_ep_c_api.h",
    "onnxruntime_error_code.h",
    "onnxruntime_float16.h"
)
foreach ($Header in $Headers) {
    Copy-Item (Join-Path $PkgDir "include\$Header") (Join-Path $VendorDir "include\onnxruntime\$Header") -Force
}
Copy-Item (Join-Path $PkgDir "lib\onnxruntime.dll") (Join-Path $VendorDir "bin\") -Force
Copy-Item (Join-Path $PkgDir "lib\onnxruntime_providers_shared.dll") (Join-Path $VendorDir "bin\") -Force
Copy-Item (Join-Path $PkgDir "LICENSE") $VendorDir -Force
Copy-Item (Join-Path $PkgDir "VERSION_NUMBER") $VendorDir -Force

Write-Host "完成。版本: $Version"
Write-Host "提示: onnxruntime.dll 依赖 VC++ 运行库（vcruntime140/msvcp140），目标机器需已安装。"
