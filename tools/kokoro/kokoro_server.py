"""Kokoro TTS HTTP 常驻服务（stdlib http.server，无 Web 框架依赖）。

服务契约（与 C++ TtsServerManager / TtsClient 对齐）:
    GET  /health  -> 200 {"status":"ok","instance_id":"<VPET_TTS_INSTANCE_ID>"}
    POST /tts     -> 200 WAV 二进制流; 400/500 JSON {"error": ...}
        请求体: {"text": str, "lang": "z"|"a", "voice": str, "speed": float}

环境变量:
    VPET_TTS_INSTANCE_ID  健康检查回显的实例标识（C++ 侧据此确认识别到本实例）
    HF_ENDPOINT           HuggingFace 镜像端点（默认 hf-mirror.com）
    HF_HOME               HuggingFace 缓存目录（默认 <项目根>/models/hf）
"""
import argparse
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

_PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

os.environ.setdefault("HF_ENDPOINT", "https://hf-mirror.com")
os.environ.setdefault("HF_HOME", os.path.join(_PROJECT_ROOT, "models", "hf"))

import numpy as np
from kokoro import KPipeline
import soundfile as sf

SUPPORTED_LANGS = {
    "a": "a", "en-us": "a", "en": "a",
    "b": "b", "en-gb": "b",
    "e": "e", "es": "e",
    "f": "f", "fr-fr": "f",
    "h": "h", "hi": "h",
    "i": "i", "it": "i",
    "p": "p", "pt-br": "p",
    "j": "j", "ja": "j",
    "z": "z", "zh": "z",
}
DEFAULT_VOICE = "zf_xiaobei"
SAMPLE_RATE = 24000

_pipelines: dict = {}
_pipelines_lock = threading.Lock()
_log_lock = threading.Lock()


def log(msg: str) -> None:
    with _log_lock:
        sys.stderr.write(f"[kokoro_server] {msg}\n")
        sys.stderr.flush()


def get_pipeline(lang: str):
    code = SUPPORTED_LANGS.get(lang, lang)
    with _pipelines_lock:
        pipe = _pipelines.get(code)
        if pipe is None:
            log(f"loading KPipeline lang_code={code!r} (once per process)")
            t0 = time.time()
            pipe = KPipeline(lang_code=code)
            log(f"KPipeline ready in {time.time() - t0:.2f}s")
            _pipelines[code] = pipe
        return pipe


class Handler(BaseHTTPRequestHandler):
    server_version = "KokoroServer/1.0"
    protocol_version = "HTTP/1.1"

    # ---- helpers -------------------------------------------------------
    def _send_json(self, code: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_wav(self, audio: np.ndarray, sample_rate: int) -> None:
        import io

        buf = io.BytesIO()
        sf.write(buf, audio, sample_rate, format="WAV")
        body = buf.getvalue()
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # ---- endpoints -----------------------------------------------------
    def do_GET(self) -> None:
        if self.path.split("?", 1)[0] != "/health":
            self._send_json(404, {"error": "not found"})
            return
        self._send_json(200, {
            "status": "ok",
            "instance_id": os.environ.get("VPET_TTS_INSTANCE_ID", ""),
        })

    def do_POST(self) -> None:
        if self.path.split("?", 1)[0] != "/tts":
            self._send_json(404, {"error": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length) if length else b""
            req = json.loads(raw.decode("utf-8"))
        except Exception as exc:  # noqa: BLE001
            self._send_json(400, {"error": f"bad request body: {exc}"})
            return

        text = str(req.get("text", "")).strip()
        if not text:
            self._send_json(400, {"error": "empty text"})
            return
        lang = str(req.get("lang", "z"))
        if lang not in SUPPORTED_LANGS:
            self._send_json(400, {"error": f"unsupported lang: {lang!r}"})
            return
        voice = str(req.get("voice", DEFAULT_VOICE))
        try:
            speed = float(req.get("speed", 1.0))
        except (TypeError, ValueError):
            speed = 1.0
        speed = min(2.0, max(0.5, speed))

        try:
            pipe = get_pipeline(lang)
            segments = list(pipe(text, voice=voice, speed=speed))
            audio = np.concatenate([seg.audio for seg in segments])
            self._send_wav(audio, SAMPLE_RATE)
        except Exception as exc:  # noqa: BLE001
            log(f"tts error: {exc!r}")
            self._send_json(500, {"error": str(exc)})

    def log_message(self, fmt, *args) -> None:  # quiet stdlib access log
        pass


def main() -> int:
    parser = argparse.ArgumentParser(description="Kokoro TTS HTTP server")
    parser.add_argument("--host", default=os.environ.get("VPET_TTS_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("VPET_TTS_PORT", "9880")))
    args = parser.parse_args()

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    log(f"listening on {args.host}:{args.port} "
        f"(instance_id={os.environ.get('VPET_TTS_INSTANCE_ID', '')!r})")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())