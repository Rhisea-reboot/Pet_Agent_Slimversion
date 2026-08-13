"""SenseVoice 离线转写脚本（子进程契约，供 VoiceInputManager 调用）。

用法:
    python tools/asr/sensevoice_transcribe.py -i <输入文件或目录> -o <输出目录> -m <模型目录>

- 模型: SenseVoiceSmall int8 ONNX（sherpa-onnx），支持中/英/日/韩/粤混合识别
- 输入: WAV（任意采样率/声道，内部 numpy 重采样到 16kHz 单声道 float32）
- 输出: <输出目录>/result.txt
    单文件输入 -> 一行纯文本（可能为空行）
    目录输入   -> 按文件名排序，每个 wav 一行
- 退出码: 0 = 成功（即使无文字）；非 0 = 失败（stderr 输出原因，不写 result.txt）
"""
import argparse
import os
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MODEL_DIR = PROJECT_ROOT / "models" / "sensevoice"
TARGET_RATE = 16000


def _load_recognizer(model_dir: Path):
    import sherpa_onnx

    onnx = model_dir / "model.int8.onnx"
    tokens = model_dir / "tokens.txt"
    if not onnx.is_file() or not tokens.is_file():
        raise FileNotFoundError(
            f"缺少模型文件（{onnx} 或 {tokens}）；请先运行: "
            f"runtime\\Scripts\\python tools\\asr\\download_sensevoice.py")
    return sherpa_onnx.OfflineRecognizer.from_sense_voice(
        model=str(onnx),
        tokens=str(tokens),
        num_threads=2,
        use_itn=True,
        debug=False,
    )


def resample_to_mono_16k(path: Path) -> np.ndarray:
    """读 wav -> 16kHz 单声道 float32 [-1, 1]；numpy 线性重采样。"""
    data, rate = sf.read(str(path), dtype="float32", always_2d=True)
    data = data.mean(axis=1)
    if rate != TARGET_RATE:
        n_out = int(round(len(data) * TARGET_RATE / rate))
        x_old = np.linspace(0.0, 1.0, num=len(data), endpoint=False)
        x_new = np.linspace(0.0, 1.0, num=n_out, endpoint=False)
        data = np.interp(x_new, x_old, data).astype(np.float32)
    return data


def transcribe_file(recognizer, path: Path) -> str:
    audio = resample_to_mono_16k(path)
    stream = recognizer.create_stream()
    stream.accept_waveform(TARGET_RATE, audio)
    recognizer.decode_stream(stream)
    return stream.result.text.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description="SenseVoice 中英混合转写")
    parser.add_argument("-i", "--input", required=True, help="输入 wav 文件或目录")
    parser.add_argument("-o", "--output", required=True, help="输出目录（自动创建）")
    parser.add_argument("-m", "--model", default=str(DEFAULT_MODEL_DIR), help="模型目录")
    args = parser.parse_args()

    src = Path(args.input)
    model_dir = Path(args.model)
    out_dir = Path(args.output)

    if not src.exists():
        sys.stderr.write(f"[asr] 输入不存在: {src}\n")
        return 2

    try:
        recognizer = _load_recognizer(model_dir)
    except Exception as exc:  # noqa: BLE001
        sys.stderr.write(f"[asr] 模型加载失败: {exc}\n")
        return 3

    if src.is_file():
        inputs = [src]
    else:
        inputs = sorted(p for p in src.iterdir() if p.suffix.lower() == ".wav")

    if not inputs:
        sys.stderr.write("[asr] 未找到任何 wav 文件\n")
        return 4

    lines: list[str] = []
    for path in inputs:
        try:
            text = transcribe_file(recognizer, path)
        except Exception as exc:  # noqa: BLE001
            sys.stderr.write(f"[asr] transcribe failed {path.name}: {exc}\n")
            return 5
        sys.stderr.write(f"[asr] {path.name} -> {text!r}\n")
        lines.append(text)

    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "result.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())