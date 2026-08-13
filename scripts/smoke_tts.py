# -*- coding: utf-8 -*-
"""TTS smoke test for the running Kokoro HTTP server."""

import json
import sys
import urllib.error
import urllib.request
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER_URL = "http://127.0.0.1:9880"
OUT_WAV = ROOT / "smoke_test.wav"


def main() -> None:
    payload = json.dumps({
        "text": "你好，我是你的桌面宠物。",
        "lang": "z",
        "voice": "zf_xiaobei",
        "speed": 1.0,
    }).encode("utf-8")
    request = urllib.request.Request(
        SERVER_URL + "/tts",
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            audio = response.read()
    except urllib.error.URLError as exc:
        raise RuntimeError(
            "Kokoro server is not reachable; start start_tts_server.bat first"
        ) from exc

    if not audio:
        raise RuntimeError("empty audio output")

    OUT_WAV.write_bytes(audio)
    with wave.open(str(OUT_WAV), "rb") as wav_file:
        if wav_file.getnframes() == 0 or wav_file.getframerate() != 24000:
            raise RuntimeError("unexpected WAV format")

    print(f"[TTS smoke] OK -> {OUT_WAV}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:  # noqa: BLE001
        print(f"[TTS smoke] FAILED: {exc}", file=sys.stderr)
        sys.exit(1)
