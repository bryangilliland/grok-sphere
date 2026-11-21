"""Board <-> companion messages. Matches firmware/main/protocol.c."""

from __future__ import annotations

import json
from typing import Any

PROTO_VERSION = 1
SAMPLE_RATE = 16000
STATES = ("idle", "listening", "thinking", "speaking", "error")
SCREENS = ("eyes", "clock")
BOARD_TYPES = ("hello", "touch", "playback_done", "pong")
TOUCH_ACTIONS = ("tap", "ptt_start", "ptt_stop")
MAX_LABEL = 31
MAX_TEXT = 95


class ProtocolError(ValueError):
    pass


def _dump(obj: dict[str, Any]) -> str:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


def _clip(text: str, limit: int) -> str:
    raw = text.encode("utf-8")
    if len(raw) <= limit:
        return text
    return raw[:limit].decode("utf-8", "ignore")


def state(name: str, label: str = "", text: str = "") -> str:
    if name not in STATES:
        raise ProtocolError(f"unknown state {name!r}")
    msg: dict[str, Any] = {"type": "state", "state": name}
    if label:
        msg["label"] = _clip(label, MAX_LABEL)
    if text:
        msg["text"] = _clip(text, MAX_TEXT)
    return _dump(msg)


def audio_start() -> str:
    return _dump({"type": "audio_start", "sample_rate": SAMPLE_RATE, "format": "pcm_s16le", "channels": 1})


def audio_end() -> str:
    return _dump({"type": "audio_end"})


def stop() -> str:
    return _dump({"type": "stop"})


def volume(value: int) -> str:
    return _dump({"type": "volume", "value": max(0, min(100, int(value)))})


def screen(name: str) -> str:
    if name not in SCREENS:
        raise ProtocolError(f"unknown screen {name!r}")
    return _dump({"type": "screen", "screen": name})


def chime() -> str:
    return _dump({"type": "chime"})


def ping() -> str:
    return _dump({"type": "ping"})


def parse_board_message(text: str) -> dict[str, Any]:
    try:
        msg = json.loads(text)
    except (TypeError, ValueError) as exc:
        raise ProtocolError("not JSON") from exc
    if not isinstance(msg, dict) or msg.get("type") not in BOARD_TYPES:
        raise ProtocolError("unknown message type")
    if msg["type"] == "touch" and msg.get("action") not in TOUCH_ACTIONS:
        raise ProtocolError("unknown touch action")
    if msg["type"] == "hello":
        if msg.get("sample_rate", SAMPLE_RATE) != SAMPLE_RATE or msg.get("channels", 1) != 1:
            raise ProtocolError("board audio format not supported")
    return msg
