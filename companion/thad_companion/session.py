"""One sphere's conversation loop: Hey Thad / PTT -> Whisper STT -> Claude -> TTS -> sphere."""

from __future__ import annotations

import asyncio
import hashlib
import logging
import threading
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from typing import Any

import numpy as np

from . import protocol as P
from .audio import (
    OWW_CHUNK,
    PLAY_FRAME_BYTES,
    Endpointer,
    Rechunker,
    apply_gain,
    duration_s,
    pcm_to_int16,
    split_frames,
)
from .claude_client import ClaudeAPIError, ClaudeConfigError
from .config import Config
from .speech_text import clean_for_speech
from .stt import clean_transcript, strip_wake_phrase
from .wakeword import HeyThadVadWake

log = logging.getLogger("session")

SendText = Callable[[str], Awaitable[None]]
SendBytes = Callable[[bytes], Awaitable[None]]


def sha8(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:8]


@dataclass
class Engines:
    wake: Any      # OpenWakeWords, HeyThadVadWake, or None
    stt: Any       # .transcribe(np.int16) -> str
    tts: Any       # .synth(text) -> 16 kHz s16le PCM bytes
    claude: Any    # .ask(text) -> str


class Session:
    AHEAD_S = 2.0

    def __init__(
        self,
        cfg: Config,
        engines: Engines,
        send_text: SendText,
        send_bytes: SendBytes,
        log_text: bool = False,
    ):
        self.cfg = cfg
        self.e = engines
        self.send_text = send_text
        self.send_bytes = send_bytes
        self.log_text = log_text
        self.state = "idle"
        self.ptt = False
        self.rechunk = Rechunker(OWW_CHUNK)
        self.endpoint = Endpointer(cfg.end_silence_s, cfg.max_seconds, cfg.no_speech_timeout_s)
        self.utterance: list[np.ndarray] = []
        self._seed_text: str = ""
        self._wake_checking = False
        self.task: asyncio.Task | None = None
        self.cancel_flag = threading.Event()
        self.playback_done = asyncio.Event()
        self.ignore_audio_until = 0.0

    def _loop_time(self) -> float:
        return asyncio.get_running_loop().time()

    async def set_state(self, name: str, label: str = "Thad", text: str = "") -> None:
        self.state = name
        await self.send_text(P.state(name, label if name != "idle" else "", text))

    async def on_connect(self) -> None:
        await self.send_text(P.volume(self.cfg.volume))
        await self.set_state("idle")

    async def on_text(self, raw: str) -> None:
        try:
            msg = P.parse_board_message(raw)
        except P.ProtocolError as exc:
            log.warning("bad message from board: %s", exc)
            return
        t = msg["type"]
        if t == "hello":
            log.info("sphere says hello: fw=%s hw=%s", msg.get("fw"), msg.get("hw"))
        elif t == "playback_done":
            self.playback_done.set()
        elif t == "touch":
            await self._on_touch(msg["action"])

    async def _on_touch(self, action: str) -> None:
        if action == "ptt_start":
            await self.cancel_current()
            await self.start_listening(ptt=True)
        elif action == "ptt_stop":
            if self.state == "listening" and self.ptt:
                await self.finish_listening("ptt_release")
        elif action == "tap":
            if self.state in ("speaking", "thinking"):
                log.info("tap: cancelling")
                await self.cancel_current()
                await self.set_state("idle")

    async def on_audio(self, data: bytes) -> None:
        if self._loop_time() < self.ignore_audio_until:
            return
        samples = apply_gain(pcm_to_int16(data), self.cfg.mic_gain)
        if self.state == "idle":
            if self.e.wake is None or self._wake_checking:
                return
            for chunk in self.rechunk.push(samples):
                if isinstance(self.e.wake, HeyThadVadWake):
                    snippet = self.e.wake.feed(chunk)
                    if snippet is not None:
                        await self._handle_vad_wake(snippet)
                        break
                else:
                    hit = self.e.wake.process(chunk)
                    if hit:
                        log.info("wake word %s (%.2f)", hit.model, hit.score)
                        await self.start_listening(ptt=False)
                        break
        elif self.state == "listening":
            self.utterance.append(samples)
            res = self.endpoint.push(samples)
            if self.ptt:
                if res.done and res.reason == "max_length":
                    await self.finish_listening("max_length")
            elif res.done:
                await self.finish_listening(res.reason)

    async def _handle_vad_wake(self, snippet) -> None:
        self._wake_checking = True
        try:
            hit = await asyncio.to_thread(self.e.wake.check_snippet, snippet)
        finally:
            self._wake_checking = False
        if hit is None or self.state != "idle":
            return
        log.info("wake phrase detected (%s, trailing=%d chars)", hit.model, len(hit.trailing_text))
        if hit.trailing_text and not hit.continuing_speech:
            await self._ask_text(hit.trailing_text)
        else:
            await self.start_listening(ptt=False, seed_text=hit.trailing_text)

    async def start_listening(self, ptt: bool, seed_text: str = "") -> None:
        self.ptt = ptt
        self._seed_text = seed_text
        self.utterance = []
        self.endpoint.reset()
        self.rechunk.reset()
        await self.set_state("listening", "Thad", "Listening...")
        if self.cfg.chime and not ptt and not seed_text:
            await self.send_text(P.chime())
            self.ignore_audio_until = self._loop_time() + 0.35

    async def finish_listening(self, reason: str) -> None:
        samples = np.concatenate(self.utterance) if self.utterance else np.zeros(0, np.int16)
        self.utterance = []
        seed = self._seed_text
        self._seed_text = ""
        heard = self.endpoint.heard or (self.ptt and samples.size > 8000) or bool(seed)
        log.info("stopped listening (%s), %.1f s", reason, samples.size / 16000)
        if not heard:
            await self.set_state("idle")
            self._reset_wake()
            return
        await self.set_state("thinking", "Thad", "Thinking...")
        self.cancel_flag.clear()
        self.task = asyncio.create_task(self._answer_audio(samples, seed))

    async def _ask_text(self, text: str) -> None:
        await self.set_state("thinking", "Thad", "Thinking...")
        self.cancel_flag.clear()
        self.task = asyncio.create_task(self._answer_text(text))

    async def _answer_audio(self, samples: np.ndarray, seed_text: str = "") -> None:
        try:
            raw = await asyncio.to_thread(self.e.stt.transcribe, samples) if samples.size > 0 else ""
            text = strip_wake_phrase(raw or "")
            if seed_text:
                text = f"{seed_text} {text}".strip() if text else seed_text
            await self._run_claude_and_speak(text)
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001
            log.exception("audio answer failed")
            await self.error(f"Error: {type(exc).__name__}")
        finally:
            self._reset_wake()

    async def _answer_text(self, text: str) -> None:
        try:
            await self._run_claude_and_speak(clean_transcript(text))
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001
            log.exception("text answer failed")
            await self.error(f"Error: {type(exc).__name__}")
        finally:
            self._reset_wake()

    async def _run_claude_and_speak(self, text: str) -> None:
        if self.log_text:
            log.info("heard: %s", text)
        else:
            log.info("heard %d chars (sha %s)", len(text), sha8(text))
        if len(text) < 2:
            await self.error("Didn't catch that", speak=True)
            return

        try:
            reply = await asyncio.to_thread(self.e.claude.ask, text)
        except ClaudeConfigError as exc:
            await self.error(str(exc), speak=False)
            return
        except ClaudeAPIError as exc:
            await self.error(str(exc), speak=True)
            return

        if self.cancel_flag.is_set():
            return

        spoken = clean_for_speech(reply, self.cfg.max_spoken_chars)
        if self.log_text:
            log.info("reply: %s", spoken)
        else:
            log.info("reply %d chars (sha %s)", len(spoken), sha8(spoken))
        await self.speak(spoken)
        await self.set_state("idle")

    async def speak(self, text: str, keep_state: str | None = None) -> None:
        pcm = await asyncio.to_thread(self.e.tts.synth, text)
        if not pcm or self.cancel_flag.is_set():
            return
        if keep_state is None:
            await self.set_state("speaking", "Thad", "Speaking")
        self.playback_done.clear()
        await self.send_text(P.audio_start())
        start = self._loop_time()
        sent = 0.0
        for frame in split_frames(pcm, PLAY_FRAME_BYTES):
            if self.cancel_flag.is_set():
                await self.send_text(P.stop())
                return
            ahead = sent - (self._loop_time() - start)
            if ahead > self.AHEAD_S:
                await asyncio.sleep(ahead - self.AHEAD_S)
            await self.send_bytes(frame)
            sent += duration_s(frame)
        await self.send_text(P.audio_end())
        remaining = max(0.0, sent - (self._loop_time() - start))
        try:
            await asyncio.wait_for(self.playback_done.wait(), timeout=remaining + 5.0)
        except asyncio.TimeoutError:
            log.warning("sphere did not confirm playback; continuing")
        self.ignore_audio_until = self._loop_time() + 0.6

    async def error(self, text: str, speak: bool = False) -> None:
        await self.set_state("error", "Thad", text)
        if speak:
            try:
                await self.speak(text + ".", keep_state="error")
            except Exception:  # noqa: BLE001
                log.exception("could not speak error")
        await asyncio.sleep(2.5)
        if self.state == "error":
            await self.set_state("idle")

    async def cancel_current(self) -> None:
        self.cancel_flag.set()
        if self.task and not self.task.done():
            self.task.cancel()
            try:
                await self.task
            except (asyncio.CancelledError, Exception):  # noqa: BLE001
                pass
        self.task = None
        await self.send_text(P.stop())
        self.cancel_flag.clear()

    def _reset_wake(self) -> None:
        self.rechunk.reset()
        if self.e.wake is not None:
            self.e.wake.reset()

    async def close(self) -> None:
        self.cancel_flag.set()
        if self.task and not self.task.done():
            self.task.cancel()
