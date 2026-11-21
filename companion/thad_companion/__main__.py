"""CLI entry point: python -m thad_companion [--check | --ask "question" | run]."""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
from pathlib import Path

from . import __version__
from .claude_client import ClaudeClient
from .config import ConfigError, load_config

HERE = Path(__file__).resolve().parent.parent


def make_engines(cfg, need_wake: bool = True):
    from .session import Engines
    from .stt import make_stt
    from .tts import make_tts
    from .wakeword import build_wake

    log = logging.getLogger("thad")
    stt = make_stt(cfg)
    wake = None
    if need_wake:
        try:
            wake = build_wake(cfg, main_stt=stt)
        except Exception as exc:  # noqa: BLE001
            log.error("wake detector unavailable (%s); push-to-talk still works", exc)
    claude = ClaudeClient(
        api_key=cfg.anthropic_api_key,
        model=cfg.claude_model,
        system_prompt=cfg.system_prompt,
        max_tokens=cfg.max_tokens,
        history_turns=cfg.history_turns,
        history_reset_s=cfg.history_reset_s,
    )
    return Engines(wake=wake, stt=stt, tts=make_tts(cfg), claude=claude)


def cmd_check(cfg, cfg_path: Path) -> int:
    ok = True
    print(f"Grok Sphere Thad Companion {__version__}")
    print(f"config: {cfg_path}")
    if cfg.anthropic_api_key:
        print(f"Claude API key: set (model={cfg.claude_model})")
    else:
        print("Claude API key: MISSING - add ANTHROPIC_API_KEY=... to companion\\.env")
        ok = False

    from .usb_transport import find_sphere_port

    port = find_sphere_port(cfg.usb_port)
    print(f"USB sphere port: {port if port else 'not detected (Wi-Fi fallback will be used if available)'}")

    for label, fn_name in (("speech-to-text", "stt"), ("text-to-speech", "tts")):
        try:
            if fn_name == "stt":
                from .stt import make_stt

                make_stt(cfg)
            else:
                from .tts import make_tts

                make_tts(cfg)
            print(f"{label}: OK")
        except Exception as exc:  # noqa: BLE001
            print(f"{label}: PROBLEM - {exc}")
            ok = False

    print("All good!" if ok else "Fix the items marked above, then run --check again.")
    return 0 if ok else 1


def cmd_ask(cfg, question: str) -> int:
    if not cfg.anthropic_api_key:
        print("Set ANTHROPIC_API_KEY in companion\\.env first.", file=sys.stderr)
        return 2
    client = ClaudeClient(
        api_key=cfg.anthropic_api_key,
        model=cfg.claude_model,
        system_prompt=cfg.system_prompt,
        max_tokens=cfg.max_tokens,
    )
    reply = client.ask(question)
    print(reply)
    return 0


async def run_services(cfg, log_text: bool) -> None:
    from .discovery import Advertiser
    from .hub import BridgeHub
    from .usb_transport import UsbTransport
    from .ws_transport import WsTransport

    hub = BridgeHub(cfg, lambda: make_engines(cfg), log_text=log_text)
    hub.engines()  # preload models so first question is fast

    tasks = []
    adv = None
    if cfg.usb_enabled:
        usb = UsbTransport(hub, cfg.usb_port, cfg.usb_baud)
        tasks.append(asyncio.create_task(usb.run()))
    if cfg.wifi_enabled:
        if cfg.advertise_mdns:
            adv = Advertiser(cfg.server_port)
            adv.start()
        ws = WsTransport(cfg, hub)
        tasks.append(asyncio.create_task(ws.run()))

    if not tasks:
        raise ConfigError("Both usb_enabled and wifi_enabled are false")

    try:
        await asyncio.gather(*tasks)
    finally:
        for t in tasks:
            t.cancel()
        if adv:
            adv.stop()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="thad_companion", description="Thad + Claude Windows Companion")
    ap.add_argument("--config", default=str(HERE / "config.yaml"))
    ap.add_argument("--check", action="store_true", help="check setup and exit")
    ap.add_argument("--ask", metavar="QUESTION", help="ask Thad (Claude) a text question and exit")
    ap.add_argument("--port", help="override USB COM port (e.g. COM5)")
    ap.add_argument("--no-usb", action="store_true")
    ap.add_argument("--no-wifi", action="store_true")
    ap.add_argument("--log-text", action="store_true", help="log recognized speech and replies")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    cfg_path = Path(args.config).expanduser()
    try:
        cfg = load_config(cfg_path)
    except (ConfigError, OSError) as exc:
        print(f"config problem: {exc}", file=sys.stderr)
        return 2

    if args.port:
        cfg.usb_port = args.port
    if args.no_usb:
        cfg.usb_enabled = False
    if args.no_wifi:
        cfg.wifi_enabled = False

    if args.check:
        return cmd_check(cfg, cfg_path)
    if args.ask:
        return cmd_ask(cfg, args.ask)

    if not cfg.anthropic_api_key:
        logging.getLogger("thad").warning(
            "ANTHROPIC_API_KEY is not set yet; add it to %s before asking questions",
            cfg_path.parent / ".env",
        )

    try:
        asyncio.run(run_services(cfg, log_text=args.log_text))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
