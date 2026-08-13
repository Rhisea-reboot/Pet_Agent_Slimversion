# Rebuild a staged GPT-SoVITS runtime without modifying the validated runtime.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PythonExecutable,
    [string]$Destination = "",
    [switch]$UseCpuTorch
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$gptRoot = Join-Path $root "GPT-SoVITS"

if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $gptRoot "runtime-rebuilt"
}

if (-not (Test-Path -LiteralPath $gptRoot)) {
    throw "GPT-SoVITS directory not found: $gptRoot"
}
if (Test-Path -LiteralPath $Destination) {
    throw "Destination already exists. Review or remove it explicitly before rebuilding: $Destination"
}

& $PythonExecutable --version
if ($LASTEXITCODE -ne 0) {
    throw "Python executable is unavailable: $PythonExecutable"
}

$pythonVersion = & $PythonExecutable -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')"
if ($pythonVersion -ne "3.9") {
    throw "GPT-SoVITS is validated with Python 3.9; received Python $pythonVersion"
}
& $PythonExecutable -c "import venv"
if ($LASTEXITCODE -ne 0) {
    throw "Python must be a full Python 3.9 installation with the venv module: $PythonExecutable"
}

# A standard Windows venv exposes Scripts/python.exe. The C++ launchers support
# that path in addition to the legacy runtime/python.exe layout.
& $PythonExecutable -m venv --copies $Destination
if ($LASTEXITCODE -ne 0) {
    throw "Failed to create staged runtime: $Destination"
}

$venvPython = Join-Path $Destination "Scripts\python.exe"
if (-not (Test-Path -LiteralPath $venvPython)) {
    throw "The staged venv does not contain its interpreter: $venvPython"
}
$runtimePython = $venvPython

& $runtimePython -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) { throw "Failed to upgrade pip" }

if ($UseCpuTorch) {
    & $runtimePython -m pip install torch==2.7.0 torchaudio==2.7.0 --index-url https://download.pytorch.org/whl/cpu
} else {
    & $runtimePython -m pip install torch==2.7.0+cu128 torchaudio==2.7.0+cu128 --index-url https://download.pytorch.org/whl/cu128
}
if ($LASTEXITCODE -ne 0) { throw "Failed to install the selected torch runtime" }

# Install the validated Chinese/English TTS and Chinese ASR closure. Japanese and
# Korean front-end packages are intentionally excluded; wordfreq and ipadic remain
# because split_lang imports them while segmenting Chinese text.
$requirements = @(
    "numpy<2.0", "scipy", "tensorboard", "librosa==0.10.2", "numba",
    "pytorch-lightning>=2.4", "ffmpeg-python", "funasr==1.0.27", "cn2an",
    "pypinyin", "g2p_en", "modelscope", "sentencepiece", "transformers>=4.43,<=4.50",
    "peft<0.18.0", "chardet", "PyYAML", "psutil", "jieba_fast", "jieba",
    "split-lang", "fast_langdetect>=0.3.1", "wordfreq", "ipadic", "wordsegment",
    "rotary_embedding_torch", "opencc", "fastapi[standard]>=0.115.2",
    "x_transformers", "torchmetrics<=1.5", "pydantic<=2.10.6",
    "ctranslate2>=4.0,<5", "av>=11", "onnxruntime-gpu", "soundfile", "tqdm"
)
& $runtimePython -m pip install @requirements
if ($LASTEXITCODE -ne 0) { throw "Failed to install the inference dependency set" }

$nltkSource = Join-Path $gptRoot "runtime\nltk_data"
if (Test-Path -LiteralPath $nltkSource) {
    Copy-Item -LiteralPath $nltkSource -Destination (Join-Path $Destination "nltk_data") -Recurse -Force
}

foreach ($tool in @("ffmpeg.exe", "ffprobe.exe")) {
    $toolSource = Join-Path $gptRoot ("runtime\" + $tool)
    if (-not (Test-Path -LiteralPath $toolSource)) { throw "Required bundled media tool is missing: $toolSource" }
    Copy-Item -LiteralPath $toolSource -Destination (Join-Path $Destination $tool)
}

& $runtimePython -c "import torch, pytorch_lightning, tensorboard, torchmetrics, fast_langdetect, wordfreq, ipadic; print(torch.__version__); print('cuda=', torch.cuda.is_available())"
if ($LASTEXITCODE -ne 0) { throw "Core inference dependency validation failed" }

Write-Host "Staged runtime created: $Destination"
Write-Host "Rename the validated runtime to a backup and swap this staged directory only after scripts/run_smoke.ps1 passes."
