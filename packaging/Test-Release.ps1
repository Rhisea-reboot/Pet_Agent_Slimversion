[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ReleaseDirectory,
    [ValidateRange(0, [int]::MaxValue)]
    [int]$CoreSizeLimitMiB = 500,
    [switch]$KokoroIncluded
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $ReleaseDirectory)) {
    throw "Release directory not found: $ReleaseDirectory"
}
$ReleaseDirectory = (Resolve-Path -LiteralPath $ReleaseDirectory).Path

$requiredFiles = @(
    "VPet.exe",
    "tts_config.json",
    "agent_dag_structure.json",
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "licenses\LGPL-3.0.txt",
    "licenses\LGPL-2.1.txt",
    "licenses\MIT.txt",
    "platforms\qwindows.dll",
    "tools\kokoro\kokoro_server.py",
    "tools\asr\sensevoice_transcribe.py",
    "models\sensevoice\model.int8.onnx",
    "models\sensevoice\tokens.txt"
)

$missing = @($requiredFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $ReleaseDirectory $_)) })
if ($missing.Count -gt 0) {
    throw ("Release self-check failed. Missing:`n" + ($missing -join [Environment]::NewLine))
}

$forbiddenPaths = @("GPT-SoVITS", "models\vosk")
if (-not $KokoroIncluded) { $forbiddenPaths += "models\hf" }
$forbidden = @($forbiddenPaths | Where-Object { Test-Path -LiteralPath (Join-Path $ReleaseDirectory $_) })
if ($forbidden.Count -gt 0) {
    throw ("Release self-check failed. Forbidden legacy/cache paths found:`n" + ($forbidden -join [Environment]::NewLine))
}

$forbiddenFiles = @(Get-ChildItem -LiteralPath $ReleaseDirectory -Recurse -Force -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in @(".pyc", ".pyo") -or $_.Name -in @("jieba.cache", "smoke_test.wav") })
if ($forbiddenFiles.Count -gt 0) {
    throw ("Release self-check failed. Generated files found:`n" + (($forbiddenFiles | ForEach-Object { $_.FullName }) -join [Environment]::NewLine))
}

$cacheDirectories = @(Get-ChildItem -LiteralPath $ReleaseDirectory -Recurse -Force -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -in @("__pycache__", ".pytest_cache", ".cache") })
if ($cacheDirectories.Count -gt 0) {
    throw ("Release self-check failed. Cache directories found:`n" + (($cacheDirectories | ForEach-Object { $_.FullName }) -join [Environment]::NewLine))
}

$animationCount = @(Get-ChildItem -LiteralPath (Join-Path $ReleaseDirectory "Animation") -Recurse -Filter *.png -File).Count
if ($animationCount -eq 0) { throw "Release self-check failed: no animation PNG files found" }

$pythonCandidates = @(
    (Join-Path $ReleaseDirectory "runtime\python.exe"),
    (Join-Path $ReleaseDirectory "runtime\Scripts\python.exe")
)
$python = $pythonCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($null -eq $python) { throw "Release self-check failed: bundled Python runtime is missing" }

# 便携性断言：发行 runtime 必须自包含，base_prefix 与解释器 DLL 不得指向外部 Python。
$releaseRuntime = Join-Path $ReleaseDirectory "runtime"
$localDllCount = @(Get-ChildItem -LiteralPath $releaseRuntime -File -Filter "python3*.dll" -ErrorAction SilentlyContinue).Count
if ($localDllCount -eq 0) { throw "Release self-check failed: python3*.dll missing beside runtime\python.exe" }
$previousNoBytecode = $env:PYTHONDONTWRITEBYTECODE
$env:PYTHONDONTWRITEBYTECODE = "1"
try {
    $basePrefix = (& $python -c "import sys; print(sys.base_prefix)" 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or
        -not $basePrefix.StartsWith($releaseRuntime, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Release self-check failed: release Python resolves to external base prefix: $basePrefix"
    }

    & $python -c "import torch, kokoro, sherpa_onnx, soundfile, numpy; print('slim runtime imports ok')"
    if ($LASTEXITCODE -ne 0) { throw "Release self-check failed: Python runtime imports failed" }

    if ($KokoroIncluded) {
        $kokoroHub = Join-Path $ReleaseDirectory "models\hf\hub\models--hexgrad--Kokoro-82M"
        if (-not (Test-Path -LiteralPath (Join-Path $kokoroHub "refs\main"))) {
            throw "Release self-check failed: Kokoro HF cache refs\main is missing"
        }
        $kokoroSnapshotCount = @(Get-ChildItem -LiteralPath (Join-Path $kokoroHub "snapshots") -Directory -ErrorAction SilentlyContinue).Count
        if ($kokoroSnapshotCount -eq 0) { throw "Release self-check failed: Kokoro HF cache has no snapshots" }
        $previousHfHome = $env:HF_HOME
        $previousHfOffline = $env:HF_HUB_OFFLINE
        $env:HF_HOME = Join-Path $ReleaseDirectory "models\hf"
        $env:HF_HUB_OFFLINE = "1"
        try {
            & $python -c "from kokoro import KPipeline; KPipeline(lang_code='z'); print('kokoro offline load ok')"
            if ($LASTEXITCODE -ne 0) { throw "Release self-check failed: Kokoro model could not be loaded from bundled cache (offline)" }
        }
        finally {
            $env:HF_HOME = $previousHfHome
            $env:HF_HUB_OFFLINE = $previousHfOffline
        }
    }
}
finally {
    $env:PYTHONDONTWRITEBYTECODE = $previousNoBytecode
}

function Get-DirectoryBytes {
    param([Parameter(Mandatory = $true)][string]$Path)

    $files = @(Get-ChildItem -LiteralPath $Path -Recurse -Force -File -ErrorAction Stop)
    if ($files.Count -eq 0) { return [int64]0 }
    return [int64](($files | Measure-Object -Property Length -Sum).Sum)
}

$totalBytes = Get-DirectoryBytes -Path $ReleaseDirectory
$runtimeBytes = Get-DirectoryBytes -Path (Join-Path $ReleaseDirectory "runtime")
$modelBytes = Get-DirectoryBytes -Path (Join-Path $ReleaseDirectory "models\sensevoice")
$kokoroBytes = 0
if ($KokoroIncluded) { $kokoroBytes = Get-DirectoryBytes -Path (Join-Path $ReleaseDirectory "models\hf") }
$coreBytes = $totalBytes - $runtimeBytes
$coreMiB = [math]::Round($coreBytes / 1MB, 2)

if ($coreMiB -gt $CoreSizeLimitMiB) {
    throw "Release self-check failed: core package is $coreMiB MiB, above the $CoreSizeLimitMiB MiB limit (runtime excluded)."
}

Write-Host ("Release self-check passed. Animation PNG count: {0}" -f $animationCount)
Write-Host (
    "Package size: {0} MiB (runtime: {1} MiB, SenseVoice: {2} MiB, Kokoro weights: {3} MiB, core: {4} MiB)" -f
    ([math]::Round($totalBytes / 1MB, 2)),
    ([math]::Round($runtimeBytes / 1MB, 2)),
    ([math]::Round($modelBytes / 1MB, 2)),
    ([math]::Round($kokoroBytes / 1MB, 2)),
    $coreMiB)
