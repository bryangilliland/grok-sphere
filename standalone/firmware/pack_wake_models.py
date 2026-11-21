#!/usr/bin/env python3
"""Pack the selected English WakeNet9 models from espressif/esp-sr into one srmodels.bin."""
import shutil
import sys
from pathlib import Path

MODELS = ("wn9_hiesp", "wn9_alexa", "wn9_jarvis_tts", "wn9_computer_tts")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: pack_wake_models.py <esp-sr-dir> <out-srmodels.bin>", file=sys.stderr)
        return 2
    sr_dir = Path(sys.argv[1])
    out_file = Path(sys.argv[2])
    stage = out_file.parent / "_srmodels_src"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True, exist_ok=True)
    for m in MODELS:
        src = sr_dir / "model" / "wakenet_model" / m
        if not src.is_dir():
            raise SystemExit(f"Missing WakeNet model in esp-sr: {src}")
        shutil.copytree(src, stage / m)
    sys.path.insert(0, str(sr_dir / "model"))
    from pack_model import pack_models
    pack_models(str(stage), out_file.name)
    (stage / out_file.name).replace(out_file)
    shutil.rmtree(stage)
    print(f"Packed {len(MODELS)} WakeNet9 models -> {out_file} ({out_file.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
