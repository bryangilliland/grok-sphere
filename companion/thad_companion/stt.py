"""Speech-to-text with faster-whisper (ships prebuilt Windows wheels, no C++ compiler needed)."""

from __future__ import annotations

import logging
import re

import numpy as np

log = logging.getLogger("stt")

WAKE_PATTERNS = (
    r"^\s*(?:hey|hi|ok|okay|yo|a)\s*[,.!]?\s*(?:thad|tad|dad|fad|chad|that)\b[,.!?\s]*",
    r"^\s*(?:thad)\b[,.!?\s]+",
)


class STTError(RuntimeError):
    pass


def clean_transcript(text: str) -> str:
    """Drop non-speech tags like [BLANK_AUDIO] or (music)."""
    t = re.sub(r"\[[^\]]*\]|\([^)]*\)|\*[^*]*\*", " ", text)
    t = re.sub(r"\s+", " ", t).strip()
    return t


def match_wake_phrase(text: str) -> tuple[bool, str]:
    """Return (matched, trailing_question) if text begins with 'Hey Thad' or a close phonetic variant."""
    cleaned = clean_transcript(text)
    if not cleaned:
        return False, ""
    for pat in WAKE_PATTERNS:
        m = re.match(pat, cleaned, flags=re.IGNORECASE)
        if m:
            rest = cleaned[m.end() :].lstrip(" ,.!?;:-").strip()
            return True, rest
    return False, cleaned


def strip_wake_phrase(text: str) -> str:
    matched, rest = match_wake_phrase(text)
    return rest if matched else clean_transcript(text)


class FasterWhisperSTT:
    def __init__(self, model: str = "base.en", device: str = "cpu", compute_type: str = "int8"):
        try:
            from faster_whisper import WhisperModel  # type: ignore
        except ImportError as exc:
            raise STTError("faster-whisper is not installed (run setup_windows.bat)") from exc
        log.info("loading faster-whisper model '%s' (%s/%s)...", model, device, compute_type)
        self._model = WhisperModel(model, device=device, compute_type=compute_type)

    def transcribe(self, samples: np.ndarray, initial_prompt: str | None = None) -> str:
        if samples.size == 0:
            return ""
        audio = samples.astype(np.float32) / 32768.0
        kwargs = {"language": "en", "beam_size": 1, "vad_filter": False}
        if initial_prompt:
            kwargs["initial_prompt"] = initial_prompt
        segments, _info = self._model.transcribe(audio, **kwargs)
        return " ".join(seg.text.strip() for seg in segments).strip()


def make_stt(cfg) -> FasterWhisperSTT:
    return FasterWhisperSTT(cfg.stt_model, cfg.stt_device, cfg.stt_compute_type)
