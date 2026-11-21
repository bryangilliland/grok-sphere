"""Advertise the Windows companion on the home Wi-Fi (mDNS / Bonjour) for Wi-Fi fallback."""

from __future__ import annotations

import logging
import socket

log = logging.getLogger("mdns")
SERVICE_TYPE = "_groksphere._tcp.local."


def local_ipv4() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


class Advertiser:
    def __init__(self, port: int):
        self.port = port
        self._zc = None
        self._info = None

    def start(self) -> str | None:
        try:
            from zeroconf import ServiceInfo, Zeroconf  # type: ignore
        except ImportError:
            log.warning("zeroconf not installed; enter this PC's IP on the sphere setup page")
            return None
        ip = local_ipv4()
        host = socket.gethostname().split(".")[0]
        self._info = ServiceInfo(
            SERVICE_TYPE,
            f"Thad Sphere on {host}.{SERVICE_TYPE}",
            addresses=[socket.inet_aton(ip)],
            port=self.port,
            properties={"path": "/sphere", "proto": "1"},
            server=f"{host}.local.",
        )
        self._zc = Zeroconf()
        self._zc.register_service(self._info)
        log.info("advertising %s on %s:%d", SERVICE_TYPE, ip, self.port)
        return ip

    def stop(self) -> None:
        if self._zc and self._info:
            self._zc.unregister_service(self._info)
            self._zc.close()
            self._zc = None
