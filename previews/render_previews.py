#!/usr/bin/env python3
"""Render 360x360 circular-screen previews of the original Grok Sphere eyes (firmware 1.0 / v2)."""

import math
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

OUT_DIR = Path(__file__).resolve().parent

SIZE = 360
# Geometry copied from firmware/main/ui.c (firmware 1.0 / v2)
EYE_W, EYE_H, EYE_DX, EYE_CY = 74, 120, 62, 160

COL_IDLE = (0xEA, 0xF2, 0xFF)       # 0xEAF2FF
COL_LISTEN = (0x3D, 0xDC, 0xFF)     # 0x3DDCFF
COL_THINK = (0x9A, 0xA7, 0xFF)      # 0x9AA7FF
COL_ERROR = (0xFF, 0x3B, 0x30)      # 0xFF3B30
COL_OFFLINE = (0x4A, 0x4F, 0x57)    # 0x4A4F57
COL_STATUS = (0x8A, 0x93, 0xA3)     # 0x8A93A3
COL_DATE = (0x9A, 0xA7, 0xFF)
COL_COND = (0xC8, 0xCE, 0xD8)
COL_HILO = (0x8A, 0x93, 0xA3)


def load_font(size: int):
    for p in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    ):
        if Path(p).exists():
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def draw_capsule(draw, cx, cy, w, h, color):
    """Vertical rounded-rect eye (radius = w/2), matching place_eye() in ui.c."""
    h = max(h, 6)
    x0, y0 = cx - w / 2, cy - h / 2
    draw.rounded_rectangle((x0, y0, x0 + w, y0 + h), radius=w / 2, fill=color)


def render_eyes(color, w=EYE_W, h=EYE_H, cy=EYE_CY, dots=False, dot_phase=0.0, label="", text=""):
    pil = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
    draw = ImageDraw.Draw(pil)
    draw_capsule(draw, 180 - EYE_DX, cy, w, h, color)
    draw_capsule(draw, 180 + EYE_DX, cy, w, h, color)

    if dots:
        for i in range(3):
            bounce = max(0.0, math.sin(dot_phase - i * 0.9))
            dx = 180 - 8 + (i - 1) * 34
            dy = 228 - 14 * bounce
            r = 8
            draw.ellipse((dx, dy, dx + 16, dy + 16), fill=COL_THINK)

    if label or text:
        font = load_font(18)
        # Status label is aligned LV_ALIGN_CENTER, y=+110 → about y=290 on a 360 screen
        txt = f"{label}\n{text}" if (label and text) else (label or text)
        draw.multiline_text((180, 290), txt, fill=COL_STATUS, font=font, anchor="ma", align="center")
    return pil


def render_clock():
    pil = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
    draw = ImageDraw.Draw(pil)
    f_big = load_font(44)
    f_med = load_font(19)
    f_sm = load_font(14)
    draw.text((180, 110), "10:42 PM", fill=(255, 255, 255), font=f_big, anchor="mm")
    draw.text((180, 160), "FRI - NOV 20", fill=COL_DATE, font=f_med, anchor="mm")
    draw.text((180, 220), "72\u00b0F", fill=(255, 255, 255), font=f_big, anchor="mm")
    draw.text((180, 270), "Mostly Clear", fill=COL_COND, font=f_med, anchor="mm")
    draw.text((180, 300), "H 78\u00b0  L 54\u00b0  |  Fort Worth", fill=COL_HILO, font=f_sm, anchor="mm")
    return pil


def mask_circle_with_bezel(im: Image.Image) -> Image.Image:
    out = Image.new("RGB", (SIZE + 24, SIZE + 24), (16, 18, 22))
    d = ImageDraw.Draw(out)
    d.ellipse((4, 4, SIZE + 20, SIZE + 20), fill=(32, 36, 42), outline=(60, 68, 76), width=2)
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, SIZE - 1, SIZE - 1), fill=255)
    out.paste(im, (12, 12), mask)
    return out


def main():
    states = [
        ("Idle", render_eyes(COL_IDLE, EYE_W, EYE_H)),
        ("Idle (Blink)", render_eyes(COL_IDLE, EYE_W, 6)),
        (
            "Listening",
            render_eyes(COL_LISTEN, 84, 140, label="Thad", text="Listening..."),
        ),
        (
            "Thinking",
            render_eyes(COL_THINK, EYE_W, 56, cy=EYE_CY - 18, dots=True, dot_phase=1.2, label="Thad", text="Thinking..."),
        ),
        (
            "Speaking",
            render_eyes(COL_IDLE, EYE_W, 88, label="Thad", text="Speaking"),
        ),
        (
            "Error",
            render_eyes(COL_ERROR, EYE_W, 90, label="Thad", text="API key missing"),
        ),
        (
            "Offline / Waiting",
            render_eyes(COL_OFFLINE, EYE_W, 36, text="Waiting for PC..."),
        ),
        ("Clock + Weather", render_clock()),
    ]

    for name, im in states:
        slug = name.lower().replace(" ", "_").replace("(", "").replace(")", "").replace("/", "_").replace("+", "")
        slug = "_".join(filter(None, slug.split("_")))
        im.save(OUT_DIR / f"state_{slug}.png")

    cell_w, cell_h = SIZE + 48, SIZE + 78
    sheet = Image.new("RGB", (cell_w * 4, cell_h * 2), (16, 18, 22))
    draw = ImageDraw.Draw(sheet)
    f_title = load_font(20)

    for idx, (name, im) in enumerate(states):
        col = idx % 4
        row = idx // 4
        x0 = col * cell_w + 12
        y0 = row * cell_h + 12
        framed = mask_circle_with_bezel(im)
        sheet.paste(framed, (x0, y0))
        draw.text((x0 + framed.width // 2, y0 + framed.height + 16), name, fill=(230, 240, 236), font=f_title, anchor="mm")

    sheet_path = OUT_DIR / "thad_sphere_preview.png"
    sheet.save(sheet_path)
    print(f"Saved {sheet_path}")


if __name__ == "__main__":
    main()
