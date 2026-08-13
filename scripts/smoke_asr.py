# -*- coding: utf-8 -*-
"""ASR smoke test for the SenseVoice subprocess contract."""

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WAV = ROOT / "smoke_test.wav"
PYTHON = ROOT / "runtime" / "Scripts" / "python.exe"
if not PYTHON.exists():
    PYTHON = ROOT / "runtime" / "python.exe"
SCRIPT = ROOT / "tools" / "asr" / "sensevoice_transcribe.py"
MODEL = ROOT / "models" / "sensevoice"


def main() -> None:
    if not WAV.exists():
        raise RuntimeError(f"missing input wav: {WAV} (run smoke_tts.py first)")
    if not PYTHON.exists():
        raise RuntimeError(f"runtime python not found: {PYTHON}")

    with tempfile.TemporaryDirectory(prefix="vpet-asr-smoke-") as output_dir:
        completed = subprocess.run(
            [
                str(PYTHON),
                str(SCRIPT),
                "-i",
                str(WAV),
                "-o",
                output_dir,
                "-m",
                str(MODEL),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(completed.stderr.strip() or "SenseVoice process failed")

        result_path = Path(output_dir) / "result.txt"
        if not result_path.exists():
            raise RuntimeError("SenseVoice did not produce result.txt")
        lines = result_path.read_text(encoding="utf-8").splitlines()
        text = lines[0].strip() if lines else ""

    print(f"[ASR smoke] recognized: {text!r}")
    if not text:
        raise RuntimeError("ASR returned empty text")
    print("[ASR smoke] OK")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:  # noqa: BLE001
        print(f"[ASR smoke] FAILED: {exc}", file=sys.stderr)
        sys.exit(1)
