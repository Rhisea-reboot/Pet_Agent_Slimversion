"""Download the SenseVoiceSmall int8 ONNX model (sherpa-onnx) into models/sensevoice/."""
import argparse
import os
import sys
from pathlib import Path

os.environ.setdefault("HF_ENDPOINT", "https://hf-mirror.com")

from huggingface_hub import hf_hub_download  # noqa: E402

REPO_ID = "csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17"
FILES = ["model.int8.onnx", "tokens.txt"]
DEFAULT_DEST = Path(__file__).resolve().parents[2] / "models" / "sensevoice"


def main() -> int:
    parser = argparse.ArgumentParser(description="Download SenseVoiceSmall int8 (sherpa-onnx)")
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST,
                        help=f"output directory (default: {DEFAULT_DEST})")
    args = parser.parse_args()

    args.dest.mkdir(parents=True, exist_ok=True)
    for name in FILES:
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