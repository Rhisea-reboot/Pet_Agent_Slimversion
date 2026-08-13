@echo off
REM ============================================
REM Kokoro TTS Server Launcher
REM 在启动桌宠前运行此脚本以启动 TTS 服务
REM ============================================

cd /d "%~dp0"

echo Starting Kokoro API server...
echo Working directory: %cd%
echo Server will listen on http://127.0.0.1:9880
echo.

set "PYTHON=runtime\Scripts\python.exe"
if not exist "%PYTHON%" set "PYTHON=runtime\python.exe"
if not exist "%PYTHON%" (
    echo Python runtime not found.
    exit /b 1
)
"%PYTHON%" tools\kokoro\kokoro_server.py --host 127.0.0.1 --port 9880

echo.
echo Server stopped.
pause