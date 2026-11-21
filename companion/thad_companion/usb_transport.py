"""USB Serial transport ("GST1" framed over COM port) for Windows / Linux."""

from __future__ import annotations

import asyncio
import contextlib
import logging
import threading
import time

from . import protocol as P
from .usb_frame import FRAME_JSON, FRAME_PCM, UsbFrameRx, encode_frame

log = logging.getLogger("usb")

ESPRESSIF_VID = 0x303A
ESPRESSIF_USJ_PID = 0x1001


def find_sphere_port(configured: str = "auto") -> str | None:
    if configured and configured.lower() != "auto":
        return configured
    try:
        from serial.tools import list_ports  # type: ignore
    except ImportError:
        return None
    ports = list(list_ports.comports())
    for p in ports:
        if p.vid == ESPRESSIF_VID and p.pid == ESPRESSIF_USJ_PID:
            return p.device
    for p in ports:
        desc = (p.description or "").lower()
        if "jtag" in desc or "esp32" in desc:
            return p.device
    return None


class UsbTransport:
    def __init__(self, hub, port_cfg: str = "auto", baud: int = 115200):
        self.hub = hub
        self.port_cfg = port_cfg
        self.baud = baud
        self._ser = None
        self._write_lock = threading.Lock()
        self._stop = asyncio.Event()

    async def send_text(self, text: str) -> None:
        frame = encode_frame(FRAME_JSON, text.encode("utf-8"))
        await asyncio.to_thread(self._write_raw, frame)

    async def send_bytes(self, pcm: bytes) -> None:
        frame = encode_frame(FRAME_PCM, pcm)
        await asyncio.to_thread(self._write_raw, frame)

    def _write_raw(self, data: bytes) -> None:
        with self._write_lock:
            if self._ser is not None:
                self._ser.write(data)
                self._ser.flush()

    async def close(self) -> None:
        self._stop.set()
        with self._write_lock:
            if self._ser is not None:
                with contextlib.suppress(Exception):
                    self._ser.close()
                self._ser = None

    async def run(self, stop: asyncio.Event | None = None) -> None:
        try:
            import serial  # type: ignore
        except ImportError:
            log.warning("pyserial not installed; USB transport disabled")
            return

        stop_ev = stop or self._stop
        while not stop_ev.is_set():
            port = find_sphere_port(self.port_cfg)
            if not port:
                await asyncio.sleep(2.0)
                continue

            try:
                ser = serial.Serial(port, self.baud, timeout=0.05, write_timeout=0.5)
            except Exception as exc:  # noqa: BLE001
                log.debug("USB port %s unavailable: %s", port, exc)
                await asyncio.sleep(2.0)
                continue

            log.info("opened sphere on USB port %s", port)
            self._ser = ser
            session = await self.hub.attach("usb", self.send_text, self.send_bytes)
            rx = UsbFrameRx()
            last_ping = 0.0

            try:
                while not stop_ev.is_set():
                    now = time.monotonic()
                    if now - last_ping >= 3.0:
                        await self.send_text(P.ping())
                        last_ping = now

                    chunk = await asyncio.to_thread(ser.read, 2048)
                    if chunk:
                        frames: list[tuple[int, bytes]] = []
                        rx.feed(chunk, lambda ft, pl: frames.append((ft, pl)))
                        for ftype, payload in frames:
                            if not self.hub.is_current(session):
                                break
                            if ftype == FRAME_PCM:
                                await session.on_audio(payload)
                            elif ftype == FRAME_JSON:
                                txt = payload.decode("utf-8", "ignore")
                                if '"type":"pong"' in txt.replace(" ", ""):
                                    continue
                                await session.on_text(txt)
                    else:
                        await asyncio.sleep(0.005)
            except Exception as exc:  # noqa: BLE001
                log.warning("USB connection on %s closed (%s)", port, exc)
            finally:
                await self.hub.detach(session)
                with self._write_lock:
                    with contextlib.suppress(Exception):
                        ser.close()
                    self._ser = None
                await asyncio.sleep(1.5)
