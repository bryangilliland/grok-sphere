"""Wi-Fi WebSocket server fallback (ws://<Windows-PC>:8765/sphere), LAN only."""

from __future__ import annotations

import asyncio
import ipaddress
import logging

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

from .config import Config

log = logging.getLogger("ws")


def is_lan_address(host: str) -> bool:
    try:
        ip = ipaddress.ip_address(host)
    except ValueError:
        return False
    if isinstance(ip, ipaddress.IPv6Address) and ip.ipv4_mapped:
        ip = ip.ipv4_mapped
    return ip.is_private or ip.is_loopback or ip.is_link_local


class WsTransport:
    def __init__(self, cfg: Config, hub):
        self.cfg = cfg
        self.hub = hub
        self.current: ServerConnection | None = None

    async def handler(self, ws: ServerConnection) -> None:
        peer = ws.remote_address[0] if ws.remote_address else ""
        path = ws.request.path if ws.request else ""
        if not is_lan_address(peer):
            log.warning("refused connection from non-LAN address %s", peer)
            await ws.close(1008, "LAN only")
            return
        if path.split("?")[0] not in ("/sphere", "/"):
            await ws.close(1008, "unknown path")
            return
        if self.current is not None:
            await self.current.close(1001, "replaced")
        self.current = ws
        log.info("sphere connected over Wi-Fi from %s", peer)

        async def send_text(t: str) -> None:
            await ws.send(t)

        async def send_bytes(b: bytes) -> None:
            await ws.send(b)

        session = await self.hub.attach("wifi", send_text, send_bytes)
        if session is None:
            await ws.close(1001, "usb active")
            if self.current is ws:
                self.current = None
            return

        try:
            async for message in ws:
                if not self.hub.is_current(session):
                    break
                if isinstance(message, bytes):
                    await session.on_audio(message)
                else:
                    await session.on_text(message)
        except ConnectionClosed:
            pass
        finally:
            await self.hub.detach(session)
            if self.current is ws:
                self.current = None
            log.info("Wi-Fi sphere disconnected")

    async def run(self, stop: asyncio.Event | None = None) -> None:
        async with serve(
            self.handler,
            self.cfg.server_host,
            self.cfg.server_port,
            max_size=2**20,
            ping_interval=20,
            ping_timeout=20,
        ) as server:
            log.info("Wi-Fi fallback listening on ws://%s:%d/sphere", self.cfg.server_host, self.cfg.server_port)
            if stop is None:
                await server.serve_forever()
            else:
                await stop.wait()
