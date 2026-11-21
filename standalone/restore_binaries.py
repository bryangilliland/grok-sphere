#!/usr/bin/env python3
"""Rebuild bin/*.bin and previews/*.png from binary_payload/ (GitHub copy stores binaries as base64 text)."""
import base64, hashlib, json, sys, tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ok = True
for item in json.loads((ROOT / "binary_payload" / "manifest.json").read_text()):
    data = base64.b64decode("".join((ROOT / p).read_text() for p in item["parts"]))
    if hashlib.sha256(data).hexdigest() != item["sha256"] or len(data) != item["bytes"]:
        print("FAIL", item["dest"]); ok = False; continue
    dest = ROOT / item["dest"]; dest.parent.mkdir(parents=True, exist_ok=True); dest.write_bytes(data)
    if dest.suffix == ".tar":
        with tarfile.open(dest) as t:
            try:
                t.extractall(ROOT, filter="data")
            except TypeError:
                t.extractall(ROOT)
        dest.unlink()
    print("OK  ", item["dest"], len(data), "bytes")
sys.exit(0 if ok else 1)
