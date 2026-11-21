#!/usr/bin/env python3
"""Rebuild firmware .bin files and preview PNGs from binary_payload/."""
from __future__ import annotations

import base64
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
MANIFEST = ROOT / "binary_payload" / "manifest.json"


def main() -> int:
    items = json.loads(MANIFEST.read_text(encoding="utf-8"))
    ok = True
    for item in items:
        dest = ROOT / item["dest"]
        dest.parent.mkdir(parents=True, exist_ok=True)
        raw = "".join(Path(ROOT / p).read_text(encoding="ascii") for p in item["parts"])
        data = base64.b64decode(raw)
        digest = hashlib.sha256(data).hexdigest()
        if digest != item["sha256"] or len(data) != item["bytes"]:
            print(f"FAIL {item['dest']}: sha256/size mismatch", file=sys.stderr)
            ok = False
            continue
        dest.write_bytes(data)
        print(f"OK   {item['dest']} ({len(data)} bytes)")
    if not ok:
        return 1
    print("All firmware bins and preview images restored.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
