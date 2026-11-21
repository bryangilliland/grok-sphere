"""Coordinates USB Serial and Wi-Fi WebSocket transports into one active Session."""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Callable

from .config import Config
from .session import Engines, SendBytes, SendText, Session

log = logging.getLogger("hub")


class BridgeHub:
    def __init__(self, cfg: Config, engines_factory: Callable[[], Engines], log_text: bool = False):
        self.cfg = cfg
        self.engines_factory = engines_factory
        self.log_text = log_text
        self._engines: Engines | None = None
        self.active_kind: str | None = None
        self.session: Session | None = None
        self._lock = asyncio.Lock()

    def engines(self) -> Engines:
        if self._engines is None:
            self._engines = self.engines_factory()
        return self._engines

    def is_current(self, session: Session) -> bool:
        return self.session is session

    async def attach(self, kind: str, send_text: SendText, send_bytes: SendBytes) -> Session | None:
        async with self._lock:
            # USB takes priority over Wi-Fi if both are connected
            if self.session is not None and self.active_kind == "usb" and kind == "wifi":
                log.info("ignoring Wi-Fi connection while USB is active")
                return None
            if self.session is not None:
                log.info("switching active sphere transport from %s to %s", self.active_kind, kind)
                await self.session.close()
            session = Session(self.cfg, self.engines(), send_text, send_bytes, self.log_text)
            self.session = session
            self.active_kind = kind
        await session.on_connect()
        return session

    async def detach(self, session: Session) -> None:
        async with self._lock:
            if self.session is session:
                await session.close()
                self.session = None
                self.active_kind = None
