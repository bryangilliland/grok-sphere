"""Configuration loader for Thad Companion (config.yaml + .env + environment variables)."""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

DEFAULT_SYSTEM_PROMPT = (
    "You are Thad, Bryan's personal voice assistant running on a desk sphere in Fort Worth, Texas. "
    "You are powered by Claude, speaking aloud through a small speaker. "
    "Keep every answer concise, natural, and conversational — usually 1 to 3 short sentences "
    "unless Bryan asks for a detailed explanation. "
    "Never use markdown, bullet lists, asterisks, emojis, or raw URLs in your reply because your text "
    "will be read aloud by text-to-speech."
)


class ConfigError(ValueError):
    pass


def _parse_simple_yaml(text: str) -> dict[str, Any]:
    """Minimal key: value fallback if PyYAML is absent in a bare test environment."""
    out: dict[str, Any] = {}
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or ":" not in line:
            continue
        k, v = line.split(":", 1)
        k, v = k.strip(), v.strip()
        if len(v) >= 2 and ((v[0] == '"' and v[-1] == '"') or (v[0] == "'" and v[-1] == "'")):
            out[k] = v[1:-1]
        elif v.lower() in ("true", "yes"):
            out[k] = True
        elif v.lower() in ("false", "no"):
            out[k] = False
        else:
            try:
                out[k] = int(v)
            except ValueError:
                try:
                    out[k] = float(v)
                except ValueError:
                    out[k] = v
    return out


def load_dotenv_file(path: Path) -> None:
    """Load simple KEY=VALUE pairs from .env without overriding real environment variables."""
    if not path.exists():
        return
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip()
        v = v.strip()
        if len(v) >= 2 and ((v[0] == '"' and v[-1] == '"') or (v[0] == "'" and v[-1] == "'")):
            v = v[1:-1]
        if k and k not in os.environ:
            os.environ[k] = v


@dataclass
class Config:
    root_dir: Path
    anthropic_api_key: str = ""
    claude_model: str = "claude-haiku-5-5"
    max_tokens: int = 220
    system_prompt: str = DEFAULT_SYSTEM_PROMPT
    history_turns: int = 8
    history_reset_s: float = 600.0

    usb_enabled: bool = True
    usb_port: str = "auto"
    usb_baud: int = 115200
    wifi_enabled: bool = True
    server_host: str = "0.0.0.0"
    server_port: int = 8765
    advertise_mdns: bool = True

    volume: int = 70
    mic_gain: float = 2.0
    chime: bool = True
    end_silence_s: float = 0.9
    no_speech_timeout_s: float = 5.0
    max_seconds: float = 15.0
    max_spoken_chars: int = 600

    wake_mode: str = "auto"
    wake_model_path: str = "wakewords/hey_thad.onnx"
    oww_fallback_model: str = ""
    wake_threshold: float = 0.5

    stt_model: str = "base.en"
    stt_wake_model: str = "tiny.en"
    stt_device: str = "cpu"
    stt_compute_type: str = "int8"

    tts_engine: str = "edge"
    edge_voice: str = "en-US-AndrewNeural"
    edge_rate: str = "+5%"
    piper_binary: str = "piper"
    piper_model: str = ""
    sapi_rate: int = 185

    extra: dict[str, Any] = field(default_factory=dict)

    def resolve_path(self, rel_or_abs: str) -> Path:
        p = Path(rel_or_abs).expanduser()
        return p if p.is_absolute() else (self.root_dir / p)


def load_config(config_path: Path) -> Config:
    root = config_path.resolve().parent
    load_dotenv_file(root / ".env")

    data: dict[str, Any] = {}
    if config_path.exists():
        raw_text = config_path.read_text(encoding="utf-8")
        try:
            import yaml  # type: ignore

            loaded = yaml.safe_load(raw_text)
        except ImportError:
            loaded = _parse_simple_yaml(raw_text)
        if loaded is not None:
            if not isinstance(loaded, dict):
                raise ConfigError(f"{config_path} must be a YAML mapping")
            data = loaded

    api_key = (os.environ.get("ANTHROPIC_API_KEY") or str(data.get("anthropic_api_key", ""))).strip()
    if api_key in ("sk-ant-YOUR_KEY_HERE", "YOUR_KEY_HERE"):
        api_key = ""

    return Config(
        root_dir=root,
        anthropic_api_key=api_key,
        claude_model=str(data.get("claude_model", "claude-haiku-5-5")),
        max_tokens=int(data.get("max_tokens", 220)),
        system_prompt=str(data.get("system_prompt", DEFAULT_SYSTEM_PROMPT)),
        history_turns=int(data.get("history_turns", 8)),
        history_reset_s=float(data.get("history_reset_s", 600.0)),
        usb_enabled=bool(data.get("usb_enabled", True)),
        usb_port=str(data.get("usb_port", "auto")),
        usb_baud=int(data.get("usb_baud", 115200)),
        wifi_enabled=bool(data.get("wifi_enabled", True)),
        server_host=str(data.get("server_host", "0.0.0.0")),
        server_port=int(data.get("server_port", 8765)),
        advertise_mdns=bool(data.get("advertise_mdns", True)),
        volume=max(0, min(100, int(data.get("volume", 70)))),
        mic_gain=float(data.get("mic_gain", 2.0)),
        chime=bool(data.get("chime", True)),
        end_silence_s=float(data.get("end_silence_s", 0.9)),
        no_speech_timeout_s=float(data.get("no_speech_timeout_s", 5.0)),
        max_seconds=float(data.get("max_seconds", 15.0)),
        max_spoken_chars=int(data.get("max_spoken_chars", 600)),
        wake_mode=str(data.get("wake_mode", "auto")),
        wake_model_path=str(data.get("wake_model_path", "wakewords/hey_thad.onnx")),
        oww_fallback_model=str(data.get("oww_fallback_model", "")),
        wake_threshold=float(data.get("wake_threshold", 0.5)),
        stt_model=str(data.get("stt_model", "base.en")),
        stt_wake_model=str(data.get("stt_wake_model", "tiny.en")),
        stt_device=str(data.get("stt_device", "cpu")),
        stt_compute_type=str(data.get("stt_compute_type", "int8")),
        tts_engine=str(data.get("tts_engine", "edge")),
        edge_voice=str(data.get("edge_voice", "en-US-AndrewNeural")),
        edge_rate=str(data.get("edge_rate", "+5%")),
        piper_binary=str(data.get("piper_binary", "piper")),
        piper_model=str(data.get("piper_model", "")),
        sapi_rate=int(data.get("sapi_rate", 185)),
    )
