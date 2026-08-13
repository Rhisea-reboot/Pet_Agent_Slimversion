@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

echo ============================================
echo  Pet Agent slimVersion - Runtime 安装
echo ============================================

set "PYTHON=F:\python.exe"
if not exist "%PYTHON%" (
    where python >nul 2>nul
    if errorlevel 1 (
        echo [错误] 未找到 F:\python.exe，且系统 python 不在 PATH 中。
        exit /b 1
    )
    set "PYTHON=python"
)

echo [1/3] 创建 venv (runtime) ...
"%PYTHON%" -m venv runtime
if errorlevel 1 (
    echo [错误] venv 创建失败。
    exit /b 1
)

echo [2/3] 安装 torch CPU 版 ...
runtime\Scripts\pip install --upgrade pip >nul
runtime\Scripts\pip install torch --index-url https://download.pytorch.org/whl/cpu
if errorlevel 1 (
    echo [错误] torch CPU 安装失败（可能需要更低版本 Python，如 3.11）。
    exit /b 1
)

echo [3/3] 安装 kokoro / sherpa-onnx / soundfile ...
runtime\Scripts\pip install kokoro soundfile sherpa-onnx >nul 2>nul
if errorlevel 1 (
    echo   - 标准安装失败（Python 3.14+ 无 numpy 1.26.4 wheel），改用兼容序列 ...
    runtime\Scripts\pip install "numpy>=2.3" || exit /b 1
    runtime\Scripts\pip install kokoro sherpa-onnx --no-deps || exit /b 1
    runtime\Scripts\pip install "misaki[zh]" spacy --only-binary :all: phonemizer num2words ^
        soundfile huggingface-hub loguru scipy transformers || exit /b 1
)

echo.
echo 安装完成。可运行以下命令验证：
echo   runtime\Scripts\python -c "import torch, kokoro, soundfile, sherpa_onnx; print('OK')"
echo   runtime\Scripts\python tools\asr\download_sensevoice.py
echo   set HF_ENDPOINT=https://hf-mirror.com ^&^& set HF_HOME=%~dp0models\hf
echo   runtime\Scripts\python -c "from kokoro import KPipeline; print('Kokoro OK')"
endlocal
