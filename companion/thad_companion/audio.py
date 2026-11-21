"""Audio helpers: PCM framing, gain, resampling, WAV I/O and a simple end-of-speech detector."""

from __future__ import annotations

import io
import wave
from collections.abc import Iterator
from dataclasses import dataclass

import numpy as np

SAMPLE_RATE = 16000
BYTES_PER_SAMPLE = 2
OWW_CHUNK = 1280                 # openWakeWord wants 80 ms chunks at 16 kHz
PLAY_FRAME_BYTES = 3200          # 100 ms of 16 kHz mono s16le per WebSocket frame


def pcm_to_int16(data: bytes) -> np.ndarray:
    """Board frames are little-endian signed 16-bit. An odd trailing byte is dropped."""
    usable = len(data) - (len(data) % BYTES_PER_SAMPLE)
    return np.frombuffer(data[:usable], dtype="<i2").astype(np.int16)


def int16_to_pcm(samples: np.ndarray) -> bytes:
    return np.asarray(samples, dtype="<i2").tobytes()


def apply_gain(samples: np.ndarray, gain: float) -> np.ndarray:
    if gain == 1.0:
        return samples.astype(np.int16)
    out = samples.astype(np.float32) * gain
    return np.clip(out, -32768, 32767).astype(np.int16)


def rms(samples: np.ndarray) -> float:
    if samples.size == 0:
        return 0.0
    f = samples.astype(np.float64)
    return float(np.sqrt(np.mean(f * f)))


def resample(samples: np.ndarray, src_rate: int, dst_rate: int = SAMPLE_RATE) -> np.ndarray:
    """Linear-interpolation resampler with a light low-pass when downsampling. Fine for speech."""
    if src_rate == dst_rate or samples.size == 0:
        return samples.astype(np.int16)
    x = samples.astype(np.float32)
    if dst_rate < src_rate:
        taps = max(1, int(round(src_rate / dst_rate)))
        if taps > 1:
            kernel = np.ones(taps, dtype=np.float32) / taps
            x = np.convolve(x, kernel, mode="same")
    n_out = int(round(len(x) * dst_rate / src_rate))
    t_out = np.arange(n_out, dtype=np.float64) * (src_rate / dst_rate)
    y = np.interp(t_out, np.arange(len(x), dtype=np.float64), x)
    return np.clip(y, -32768, 32767).astype(np.int16)


def split_frames(pcm: bytes, frame_bytes: int = PLAY_FRAME_BYTES) -> Iterator[bytes]:
    """Split PCM into frames on sample boundaries (frame_bytes must be even)."""
    if frame_bytes <= 0 or frame_bytes % BYTES_PER_SAMPLE:
        raise ValueError("frame_bytes must be a positive even number")
    usable = len(pcm) - (len(pcm) % BYTES_PER_SAMPLE)
    for i in range(0, usable, frame_bytes):
        yield pcm[i:i + frame_bytes]


def duration_s(pcm: bytes) -> float:
    return len(pcm) / (SAMPLE_RATE * BYTES_PER_SAMPLE)


class Rechunker:
    """Collects arbitrary-size frames and hands out fixed-size int16 chunks."""

    def __init__(self, chunk: int = OWW_CHUNK):
        self.chunk = chunk
        self._buf = np.zeros(0, dtype=np.int16)

    def push(self, samples: np.ndarray) -> list[np.ndarray]:
        self._buf = np.concatenate([self._buf, samples.astype(np.int16)])
        out = []
        while len(self._buf) >= self.chunk:
            out.append(self._buf[:self.chunk])
            self._buf = self._buf[self.chunk:]
        return out

    def reset(self) -> None:
        self._buf = np.zeros(0, dtype=np.int16)


def read_wav_16k_mono(data: bytes) -> np.ndarray:
    """Read WAV bytes (any rate, mono/stereo, 16-bit) into 16 kHz mono int16."""
    with wave.open(io.BytesIO(data), "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError("only 16-bit WAV is supported")
        rate, ch = w.getframerate(), w.getnchannels()
        frames = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    if ch > 1:
        frames = frames.reshape(-1, ch).mean(axis=1).astype(np.int16)
    return resample(frames, rate, SAMPLE_RATE)


def write_wav(samples: np.ndarray, rate: int = SAMPLE_RATE) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(int16_to_pcm(samples))
    return buf.getvalue()


@dataclass
class EndpointResult:
    done: bool
    heard_speech: bool
    reason: str = ""


class Endpointer:
    """Decides when a spoken question is over, from 16 kHz frames.

    Uses an adaptive noise floor: speech = frame RMS well above the floor. Recording ends after
    `end_silence_s` of quiet once speech was heard, after `no_speech_timeout_s` without speech,
    or at `max_seconds`.
    """

    def __init__(self, end_silence_s: float = 0.9, max_seconds: float = 15.0,
                 no_speech_timeout_s: float = 5.0, min_speech_rms: float = 300.0, ratio: float = 2.5):
        self.end_silence_s = end_silence_s
        self.max_seconds = max_seconds
        self.no_speech_timeout_s = no_speech_timeout_s
        self.min_speech_rms = min_speech_rms
        self.ratio = ratio
        self.reset()

    def reset(self, noise_floor: float = 150.0) -> None:
        self.noise = noise_floor
        self.elapsed = 0.0
        self.silence = 0.0
        self.speech_time = 0.0
        self.heard = False

    def push(self, samples: np.ndarray) -> EndpointResult:
        dt = len(samples) / SAMPLE_RATE
        self.elapsed += dt
        level = rms(samples)
        is_speech = level > max(self.min_speech_rms, self.noise * self.ratio)
        if is_speech:
            self.speech_time += dt
            if self.speech_time >= 0.12:
                self.heard = True
            self.silence = 0.0
        else:
            self.silence += dt
            self.noise = 0.95 * self.noise + 0.05 * level   # follow the room noise slowly
        if self.elapsed >= self.max_seconds:
            return EndpointResult(True, self.heard, "max_length")
        if self.heard and self.silence >= self.end_silence_s:
            return EndpointResult(True, True, "silence")
        if not self.heard and self.elapsed >= self.no_speech_timeout_s:
            return EndpointResult(True, False, "no_speech")
        return EndpointResult(False, self.heard)
