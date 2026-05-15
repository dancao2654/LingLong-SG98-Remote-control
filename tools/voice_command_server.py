#!/usr/bin/env python3
"""Tiny SG98 voice-command endpoint for the Waveshare remote.

The remote POSTs a short WAV to /command. This server transcribes it with the
OpenAI Audio Transcriptions API when OPENAI_API_KEY is set, maps the transcript
to a small SG98 command allowlist, and returns JSON the firmware understands.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
import urllib.error
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


DEFAULT_MODEL = os.environ.get("OPENAI_TRANSCRIBE_MODEL", "gpt-4o-mini-transcribe")
OPENAI_TRANSCRIBE_URL = "https://api.openai.com/v1/audio/transcriptions"
MAX_AUDIO_BYTES = 512 * 1024


def multipart_form(fields: dict[str, str], files: dict[str, tuple[str, bytes, str]]) -> tuple[bytes, str]:
    boundary = f"----sg98-{uuid.uuid4().hex}"
    chunks: list[bytes] = []
    for name, value in fields.items():
        chunks.append(f"--{boundary}\r\n".encode())
        chunks.append(f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode())
        chunks.append(value.encode())
        chunks.append(b"\r\n")
    for name, (filename, data, content_type) in files.items():
        chunks.append(f"--{boundary}\r\n".encode())
        chunks.append(
            f'Content-Disposition: form-data; name="{name}"; filename="{filename}"\r\n'.encode()
        )
        chunks.append(f"Content-Type: {content_type}\r\n\r\n".encode())
        chunks.append(data)
        chunks.append(b"\r\n")
    chunks.append(f"--{boundary}--\r\n".encode())
    return b"".join(chunks), boundary


def transcribe_openai(audio: bytes, model: str) -> str:
    api_key = os.environ.get("OPENAI_API_KEY")
    if not api_key:
        raise RuntimeError("OPENAI_API_KEY is not set")

    body, boundary = multipart_form(
        {"model": model},
        {"file": ("sg98-voice.wav", audio, "audio/wav")},
    )
    request = urllib.request.Request(
        OPENAI_TRANSCRIBE_URL,
        data=body,
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": f"multipart/form-data; boundary={boundary}",
        },
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=45) as response:
        payload = json.loads(response.read().decode("utf-8"))
    return str(payload.get("text", "")).strip()


def transcribe_file(audio: bytes, model: str, save_dir: Path | None) -> str:
    if save_dir is not None:
        save_dir.mkdir(parents=True, exist_ok=True)
        path = save_dir / "last-sg98-voice.wav"
        path.write_bytes(audio)
    return transcribe_openai(audio, model)


def map_intent(transcript: str) -> dict[str, object]:
    text = transcript.lower().strip()
    compact = re.sub(r"[^a-z0-9. ]+", " ", text)
    compact = re.sub(r"\s+", " ", compact).strip()

    rpm = re.search(r"(?:set )?(?:rpm|speed)(?: to)? (\d{2,4})", compact)
    if rpm:
        return {"command": "set_rpm", "value": int(rpm.group(1)), "transcript": transcript}

    dose = re.search(r"(?:set )?(?:dose|weight)(?: to)? ([0-9]+(?:\.[0-9]+)?) ?(?:g|gram|grams)?", compact)
    if dose:
        grams = float(dose.group(1))
        value = int(round(grams * 1000)) if grams < 200 else int(round(grams))
        return {"command": "set_weight", "value": value, "transcript": transcript}

    if any(word in compact for word in ("stop", "halt", "cancel", "abort")):
        return {"command": "stop", "transcript": transcript}
    if "manual" in compact:
        return {"command": "manual", "transcript": transcript}
    if "grind by weight" in compact or "weight mode" in compact or "gbw" in compact:
        return {"command": "gbw", "transcript": transcript}
    if "home" in compact:
        return {"command": "home", "transcript": transcript}
    if "menu" in compact or "settings" in compact:
        return {"command": "menu", "transcript": transcript}
    if "start" in compact:
        return {"command": "start", "transcript": transcript}

    return {"command": "unknown", "transcript": transcript}


class VoiceHandler(BaseHTTPRequestHandler):
    model = DEFAULT_MODEL
    save_dir: Path | None = None

    def do_POST(self) -> None:  # noqa: N802
        if self.path not in ("/command", "/sg98/voice"):
            self.send_json(404, {"error": "use /command"})
            return

        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > MAX_AUDIO_BYTES:
            self.send_json(413, {"error": "bad audio length"})
            return

        audio = self.rfile.read(length)
        try:
            transcript = transcribe_file(audio, self.model, self.save_dir)
            if not transcript:
                self.send_json(422, {"error": "empty transcript"})
                return
            self.send_json(200, map_intent(transcript))
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            self.send_json(exc.code, {"error": detail[:300]})
        except Exception as exc:  # keep endpoint failure visible to firmware
            self.send_json(500, {"error": str(exc)})

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"[voice] {self.address_string()} {fmt % args}", file=sys.stderr)

    def send_json(self, status: int, payload: dict[str, object]) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main() -> int:
    parser = argparse.ArgumentParser(description="SG98 voice command endpoint")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--save-audio", type=Path, default=None)
    args = parser.parse_args()

    VoiceHandler.model = args.model
    VoiceHandler.save_dir = args.save_audio
    server = ThreadingHTTPServer((args.host, args.port), VoiceHandler)
    print(f"SG98 voice endpoint listening on http://{args.host}:{args.port}/command")
    print(f"Transcription model: {args.model}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
