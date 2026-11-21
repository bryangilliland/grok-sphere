"""Framed binary transport over USB-Serial-JTAG ("GST1" + CRC16-CCITT).

Matches firmware/main/usb_frame.c byte-for-byte.
"""

from __future__ import annotations

import struct
from collections.abc import Callable

MAGIC = b"GST1"
HDR_SIZE = 8
CRC_SIZE = 2
MAX_PAYLOAD = 4096
FRAME_JSON = 1
FRAME_PCM = 2


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode_frame(frame_type: int, payload: bytes) -> bytes:
    if frame_type not in (FRAME_JSON, FRAME_PCM):
        raise ValueError(f"invalid frame_type: {frame_type}")
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload too large: {len(payload)}")
    header = struct.pack("<BBH", frame_type, 0, len(payload))
    crc = crc16_ccitt(header + payload)
    return MAGIC + header + payload + struct.pack("<H", crc)


class UsbFrameRx:
    """Streaming decoder that extracts valid CRC-checked GST1 frames from arbitrary byte chunks."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def reset(self) -> None:
        self._buf.clear()

    def feed(self, data: bytes, on_frame: Callable[[int, bytes], None]) -> None:
        if not data:
            return
        self._buf.extend(data)
        while True:
            idx = self._buf.find(MAGIC)
            if idx < 0:
                # Keep up to 3 trailing bytes in case MAGIC spans chunks
                if len(self._buf) > 3:
                    del self._buf[:-3]
                return
            if idx > 0:
                del self._buf[:idx]
            if len(self._buf) < HDR_SIZE:
                return
            ftype, _flags, plen = struct.unpack_from("<BBH", self._buf, 4)
            if ftype not in (FRAME_JSON, FRAME_PCM) or plen > MAX_PAYLOAD:
                del self._buf[:1]
                continue
            total = HDR_SIZE + plen + CRC_SIZE
            if len(self._buf) < total:
                return
            body = bytes(self._buf[4 : HDR_SIZE + plen])
            (got_crc,) = struct.unpack_from("<H", self._buf, HDR_SIZE + plen)
            if crc16_ccitt(body) == got_crc:
                payload = bytes(self._buf[HDR_SIZE : HDR_SIZE + plen])
                del self._buf[:total]
                on_frame(ftype, payload)
            else:
                del self._buf[:1]
