"""Wake-word detection for 'Hey Thad'.

Supports:
  1. Custom openWakeWord ONNX/TFLite model (if wakewords/hey_thad.onnx exists)
  2. Built-in openWakeWord fallback (optional, e.g. 'hey_jarvis')
  3. Built-in VAD + fast Whisper 'Hey Thad' phrase detector (works out of the box with no training)
  4. Push-to-talk on the sphere screen/BOOT button (always active)
"""

from __future__ import annotations

import logging
from collections import deque
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from .audio import SAMPLE_RATE, rms
from .stt import match_wake_phrase

log = logging.getLogger("wakeword")


@dataclass
class WakeHit:
    bot_name: str
    model: str
    score: float
    trailing_text: str = ""
    continuing_speech: bool = False


class OpenWakeWords:
    def __init__(self, model_paths: list[str], threshold: float = 0.5, model_factory=None):
        self.threshold = threshold
        self.model_factory = model_factory or _default_oww_factory
        self._model = self.model_factory(model_paths) if model_paths else None
        self._cooldown = 0

    @property
    def active(self) -> bool:
        return self._model is not None

    def process(self, chunk: np.ndarray) -> WakeHit | None:
        if self._model is None:
            return None
        scores = self._model.predict(chunk)
        if self._cooldown > 0:
            self._cooldown -= 1
            return None
        best: WakeHit | None = None
        for key, score in scores.items():
            if score >= self.threshold and (best is None or score > best.score):
                best = WakeHit("Thad", key, float(score))
        if best:
            self._cooldown = 15
        return best

    def reset(self) -> None:
        if self._model is not None and hasattr(self._model, "reset"):
            self._model.reset()
        self._cooldown = 0


def _default_oww_factory(paths: list[str]):
    import openwakeword  # type: ignore
    from openwakeword.model import Model  # type: ignore

    builtin = [p for p in paths if not p.endswith((".onnx", ".tflite"))]
    if builtin:
        try:
            openwakeword.utils.download_models(model_names=builtin)
        except Exception as exc:  # noqa: BLE001
            log.warning("could not download openWakeWord models (%s)", exc)
    return Model(wakeword_models=paths, inference_framework="onnx")


@dataclass
class SpeechSnippet:
    samples: np.ndarray
    continuing: bool


class HeyThadVadWake:
    """Buffers short speech bursts so a fast Whisper pass can detect 'Hey Thad'."""

    def __init__(
        self,
        stt_engine,
        min_speech_rms: float = 320.0,
        ratio: float = 2.6,
        end_silence_s: float = 0.35,
        max_snippet_s: float = 1.8,
        min_speech_s: float = 0.24,
    ):
        self.stt = stt_engine
        self.min_speech_rms = min_speech_rms
        self.ratio = ratio
        self.end_silence_s = end_silence_s
        self.max_snippet_s = max_snippet_s
        self.min_speech_s = min_speech_s
        self._preroll: deque[np.ndarray] = deque(maxlen=4)  # ~320 ms pre-roll
        self.reset()

    def reset(self) -> None:
        self.noise = 150.0
        self.in_speech = False
        self.speech_s = 0.0
        self.silence_s = 0.0
        self.total_s = 0.0
        self._buf: list[np.ndarray] = []
        self._preroll.clear()

    def feed(self, chunk: np.ndarray) -> SpeechSnippet | None:
        """Feed 1280 int16 samples (80 ms). Returns a candidate snippet when ready to check."""
        dt = len(chunk) / SAMPLE_RATE
        level = rms(chunk)
        thresh = max(self.min_speech_rms, self.noise * self.ratio)
        is_speech = level > thresh

        if not self.in_speech:
            self._preroll.append(chunk)
            if is_speech:
                self.in_speech = True
                self._buf = list(self._preroll)
                self.speech_s = dt
                self.silence_s = 0.0
                self.total_s = sum(len(c) for c in self._buf) / SAMPLE_RATE
            else:
                self.noise = 0.95 * self.noise + 0.05 * level
            return None

        self._buf.append(chunk)
        self.total_s += dt
        if is_speech:
            self.speech_s += dt
            self.silence_s = 0.0
        else:
            self.silence_s += dt

        if self.silence_s >= self.end_silence_s or self.total_s >= self.max_snippet_s:
            continuing = self.silence_s < self.end_silence_s
            samples = np.concatenate(self._buf) if self._buf else np.zeros(0, np.int16)
            ok = self.speech_s >= self.min_speech_s
            self.in_speech = False
            self._buf = []
            self._preroll.clear()
            if ok and samples.size > 0:
                return SpeechSnippet(samples=samples, continuing=continuing)
        return None

    def check_snippet(self, snippet: SpeechSnippet) -> WakeHit | None:
        text = self.stt.transcribe(snippet.samples, initial_prompt="Hey Thad.")
        matched, rest = match_wake_phrase(text)
        if not matched:
            return None
        return WakeHit(
            bot_name="Thad",
            model="hey_thad_stt",
            score=1.0,
            trailing_text=rest,
            continuing_speech=snippet.continuing,
        )


def build_wake(cfg, main_stt=None):
    """Build the configured wake detector (or None for PTT-only)."""
    mode = (cfg.wake_mode or "auto").lower()
    if mode == "ptt_only":
        return None

    custom_path = cfg.resolve_path(cfg.wake_model_path) if cfg.wake_model_path else None
    if mode in ("auto", "openwakeword") and custom_path and custom_path.exists():
        log.info("using custom openWakeWord model: %s", custom_path)
        return OpenWakeWords([str(custom_path)], cfg.wake_threshold)

    if mode == "openwakeword" and cfg.oww_fallback_model:
        log.info("using openWakeWord built-in model: %s", cfg.oww_fallback_model)
        return OpenWakeWords([cfg.oww_fallback_model], cfg.wake_threshold)

    if mode in ("auto", "hey_thad_stt"):
        from .stt import FasterWhisperSTT

        wake_stt = main_stt
        if wake_stt is None or (cfg.stt_wake_model and cfg.stt_wake_model != cfg.stt_model):
            wake_stt = FasterWhisperSTT(cfg.stt_wake_model, cfg.stt_device, cfg.stt_compute_type)
        log.info("using built-in 'Hey Thad' speech wake detector (model=%s)", cfg.stt_wake_model)
        return HeyThadVadWake(wake_stt)

    if cfg.oww_fallback_model:
        return OpenWakeWords([cfg.oww_fallback_model], cfg.wake_threshold)
    return None
