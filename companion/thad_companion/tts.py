"""Text-to-speech for Windows (16 kHz mono s16le PCM for the sphere).

  1. edge-tts (default, free neural Microsoft voices; decoded in-memory via PyAV, no ffmpeg.exe needed)
  2. pyttsx3 (offline Windows SAPI5 fallback, no internet required)
  3. piper (optional local neural TTS binary)
"""

from __future__ import annotations

import asyncio
import io
import json
import logging
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np

from .audio import SAMPLE_RATE, int16_to_pcm, read_wav_16k_mono, resample

log = logging.getLogger("tts")


class TTSError(RuntimeError):
    pass


def decode_audio_bytes_to_16k_pcm(audio_bytes: bytes) -> bytes:
    """Decode MP3/OGG/WAV bytes in memory to 16 kHz mono s16le PCM using PyAV."""
    try:
        import av  # type: ignore
    except ImportError as exc:
        raise TTSError("PyAV (av) not installed; needed to decode edge-tts audio") from exc

    container = av.open(io.BytesIO(audio_bytes))
    resampler = av.AudioResampler(format="s16", layout="mono", rate=SAMPLE_RATE)
    chunks: list[bytes] = []
    for frame in container.decode(audio=0):
        for rframe in resampler.resample(frame):
            arr = rframe.to_ndarray()
            chunks.append(np.asarray(arr, dtype="<i2").tobytes())
    for rframe in resampler.resample(None):
        arr = rframe.to_ndarray()
        chunks.append(np.asarray(arr, dtype="<i2").tobytes())
    return b"".join(chunks)


class EdgeTTS:
    def __init__(self, voice: str = "en-US-AndrewNeural", rate: str = "+5%"):
        try:
            import edge_tts  # noqa: F401
        except ImportError as exc:
            raise TTSError("edge-tts is not installed (run setup_windows.bat)") from exc
        self.voice = voice
        self.rate = rate

    async def _synth_mp3(self, text: str) -> bytes:
        import edge_tts  # type: ignore

        comm = edge_tts.Communicate(text, voice=self.voice, rate=self.rate)
        out = bytearray()
        async for chunk in comm.stream():
            if chunk.get("type") == "audio":
                out.extend(chunk["data"])
        return bytes(out)

    def synth(self, text: str) -> bytes:
        mp3 = asyncio.run(self._synth_mp3(text))
        if not mp3:
            raise TTSError("edge-tts returned no audio")
        return decode_audio_bytes_to_16k_pcm(mp3)


class Pyttsx3TTS:
    def __init__(self, rate: int = 185):
        try:
            import pyttsx3  # noqa: F401
        except ImportError as exc:
            raise TTSError("pyttsx3 is not installed") from exc
        self.rate = rate

    def synth(self, text: str) -> bytes:
        import pyttsx3  # type: ignore

        with tempfile.TemporaryDirectory() as d:
            wav = Path(d) / "speech.wav"
            engine = pyttsx3.init()
            engine.setProperty("rate", self.rate)
            engine.save_to_file(text, str(wav))
            engine.runAndWait()
            if not wav.exists():
                raise TTSError("Windows SAPI TTS failed to write WAV")
            return int16_to_pcm(read_wav_16k_mono(wav.read_bytes()))


class PiperTTS:
    def __init__(self, binary: str, model: str):
        self.binary = shutil.which(binary) or binary
        self.model = str(Path(os.path.expanduser(model)))
        if not shutil.which(self.binary):
            raise TTSError(f"{binary} not found")
        if not Path(self.model).exists():
            raise TTSError(f"Piper voice not found at {self.model}")
        self.rate = 22050
        meta = Path(self.model + ".json")
        if meta.exists():
            try:
                self.rate = int(json.loads(meta.read_text())["audio"]["sample_rate"])
            except (ValueError, KeyError, TypeError):
                pass

    def synth(self, text: str) -> bytes:
        res = subprocess.run(
            [self.binary, "--model", self.model, "--output_raw"],
            input=text.encode("utf-8"),
            capture_output=True,
            timeout=60,
        )
        if res.returncode != 0:
            raise TTSError(f"piper failed ({res.returncode})")
        raw = np.frombuffer(res.stdout[: len(res.stdout) // 2 * 2], dtype="<i2")
        return int16_to_pcm(resample(raw, self.rate, SAMPLE_RATE))


class FallbackTTS:
    """Tries primary TTS (e.g. edge-tts) and falls back to offline SAPI if network fails."""

    def __init__(self, primary, fallback=None):
        self.primary = primary
        self.fallback = fallback

    def synth(self, text: str) -> bytes:
        try:
            return self.primary.synth(text)
        except Exception as exc:  # noqa: BLE001
            if self.fallback is None:
                raise
            log.warning("primary TTS failed (%s); falling back to offline voice", exc)
            return self.fallback.synth(text)


def make_tts(cfg):
    eng = (cfg.tts_engine or "edge").lower()
    if eng == "piper":
        return PiperTTS(cfg.piper_binary, cfg.piper_model)
    if eng == "pyttsx3":
        return Pyttsx3TTS(cfg.sapi_rate)

    primary = EdgeTTS(cfg.edge_voice, cfg.edge_rate)
    try:
        fallback = Pyttsx3TTS(cfg.sapi_rate)
    except TTSError:
        fallback = None
    return FallbackTTS(primary, fallback)
