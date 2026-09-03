"""Download the SenseVoiceSmall ONNX model (sherpa-onnx) into models/sensevoice/.

By default this downloads the full-precision model.onnx (~938 MB), which avoids
the accuracy loss of int8 quantization. Pass --int8 to download the smaller
int8 model (~228 MB) instead.
"""
import argparse
import os
import sys
from pathlib import Path

os.environ.setdefault("HF_ENDPOINT", "https://hf-mirror.com")

from huggingface_hub import hf_hub_download  # noqa: E402

REPO_ID = "csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17"
FULL_PRECISION_FILES = ["model.onnx", "tokens.txt"]
INT8_FILES = ["model.int8.onnx", "tokens.txt"]
DEFAULT_DEST = Path(__file__).resolve().parents[2] / "models" / "sensevoice"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Download SenseVoiceSmall (sherpa-onnx); default full precision")
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST,
                        help=f"output directory (default: {DEFAULT_DEST})")
    parser.add_argument("--int8", action="store_true",
                        help="download the int8 quantized model instead of full precision")
    args = parser.parse_args()

    files = INT8_FILES if args.int8 else FULL_PRECISION_FILES
    args.dest.mkdir(parents=True, exist_ok=True)
    for name in files:
        target = args.dest / name
        if target.is_file() and target.stat().st_size > 0:
            print(f"[skip] {name} already exists")
            continue
        print(f"[download] {name}")
        path = hf_hub_download(repo_id=REPO_ID, filename=name, local_dir=args.dest)
        print(f"  -> {path}")
    print(f"done: {args.dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())