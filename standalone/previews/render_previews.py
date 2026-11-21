#!/usr/bin/env python3
"""Render 360x360 previews of the standalone Grok Sphere screens.
Geometry and colors are copied from firmware/main/ui.c (original firmware 1.0 eyes)."""
import math
from pathlib import Path
import qrcode
from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).resolve().parent
SIZE = 360
EYE_W, EYE_H, EYE_DX, EYE_CY = 74, 120, 62, 160
REPLY_W, REPLY_H, REPLY_Y = 232, 76, 248
C_IDLE, C_LISTEN, C_THINK = (0xEA, 0xF2, 0xFF), (0x3D, 0xDC, 0xFF), (0x9A, 0xA7, 0xFF)
C_ERR, C_OFF, C_STATUS, C_REPLY = (0xFF, 0x3B, 0x30), (0x4A, 0x4F, 0x57), (0x8A, 0x93, 0xA3), (0xC8, 0xCE, 0xD8)
MONT = "/usr/share/fonts/truetype/sand-box/google/Montserrat/Montserrat-VariableFont_wght.ttf"


def font(size):
    try:
        f = ImageFont.truetype(MONT, size)
        try:
            f.set_variation_by_name("Medium")
        except Exception:
            pass
        return f
    except OSError:
        return ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", size)


def capsule(d, cx, cy, w, h, col):
    h = max(h, 6)
    d.rounded_rectangle((cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), radius=w / 2, fill=col)


def wrap(d, text, f, width):
    lines, line = [], ""
    for word in text.split():
        t = (line + " " + word).strip()
        if d.textlength(t, font=f) <= width:
            line = t
        else:
            lines.append(line)
            line = word
    if line:
        lines.append(line)
    return lines


def eyes(col, w=EYE_W, h=EYE_H, cy=EYE_CY, dots=False, status="", reply=""):
    im = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
    d = ImageDraw.Draw(im)
    capsule(d, 180 - EYE_DX, cy, w, h, col)
    capsule(d, 180 + EYE_DX, cy, w, h, col)
    if dots:
        for i in range(3):
            y = 228 - 14 * max(0.0, math.sin(1.2 - i * 0.9))
            x = 180 - 8 + (i - 1) * 34
            d.ellipse((x, y, x + 16, y + 16), fill=C_THINK)
    if reply:
        f = font(20)
        box = Image.new("RGB", (REPLY_W, REPLY_H), (0, 0, 0))
        bd = ImageDraw.Draw(box)
        y = 0
        for ln in wrap(bd, reply, f, REPLY_W):
            bd.text((REPLY_W / 2, y), ln, font=f, fill=C_REPLY, anchor="ma")
            y += 24
        im.paste(box, (180 - REPLY_W // 2, REPLY_Y))
    elif status:
        f = font(20)
        lines = wrap(d, status, f, 250)[:2]
        d.multiline_text((180, 290 - 12 * len(lines)), "\n".join(lines), font=f, fill=C_STATUS, anchor="ma", align="center")
    return im


def qr_screen(title, data, body):
    im = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
    d = ImageDraw.Draw(im)
    d.text((180, 55), title, font=font(20), fill=(255, 255, 255), anchor="mm")
    q = qrcode.QRCode(border=0, box_size=4)
    q.add_data(data)
    q.make(fit=True)
    qi = q.make_image(fill_color="black", back_color="white").convert("RGB").resize((140, 140), Image.NEAREST)
    frame = Image.new("RGB", (152, 152), (255, 255, 255))
    frame.paste(qi, (6, 6))
    im.paste(frame, (180 - 76, 155 - 76))
    d.multiline_text((180, 280), body, font=font(14), fill=C_REPLY, anchor="mm", align="center", spacing=4)
    return im


def clock():
    im = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
    d = ImageDraw.Draw(im)
    d.text((180, 110), "11:42 AM", font=font(48), fill=(255, 255, 255), anchor="mm")
    d.text((180, 160), "SAT - NOV 21", font=font(20), fill=C_THINK, anchor="mm")
    d.text((180, 220), "68\u00b0F", font=font(48), fill=(255, 255, 255), anchor="mm")
    d.text((180, 270), "Partly cloudy", font=font(20), fill=C_REPLY, anchor="mm")
    d.text((180, 300), "H 72\u00b0  L 51\u00b0  |  Fort Worth", font=font(14), fill=C_STATUS, anchor="mm")
    return im


def bezel(im):
    out = Image.new("RGB", (SIZE + 24, SIZE + 24), (16, 18, 22))
    d = ImageDraw.Draw(out)
    d.ellipse((4, 4, SIZE + 20, SIZE + 20), fill=(32, 36, 42), outline=(60, 68, 76), width=2)
    m = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(m).ellipse((0, 0, SIZE - 1, SIZE - 1), fill=255)
    out.paste(im, (12, 12), m)
    return out


def main():
    states = [
        ("1 Setup hotspot", qr_screen("Sphere setup", "WIFI:T:WPA;S:GrokSphere-3A7C;P:48213907;;",
                                      "Scan with your phone camera, or join\nGrokSphere-3A7C  /  48213907\nthen open  http://192.168.4.1")),
        ("2 Key only (on home Wi-Fi)", qr_screen("Add your Claude key", "http://192.168.1.50/",
                                                 "Phone on the same Wi-Fi:\nscan, or open http://192.168.1.50\n(or http://grok-sphere.local)")),
        ("3 Idle", eyes(C_IDLE)),
        ("4 Listening (tap)", eyes(C_LISTEN, 84, 140, status="Listening...")),
        ("5 Thinking", eyes(C_THINK, EYE_W, 56, EYE_CY - 18, dots=True, reply="What's a good name for a fantasy football team?")),
        ("6 Speaking + reply text", eyes(C_IDLE, EYE_W, 96, reply="How about the Fort Worth Fumbles? Self-aware, local, and honestly a bit of a warning.")),
        ("7 Error", eyes(C_ERR, EYE_W, 90, status="Claude key rejected")),
        ("8 Clock + weather (hold)", clock()),
    ]
    for name, im in states:
        slug = "_".join(name.lower().replace("(", "").replace(")", "").replace("+", "").split()[1:])
        im.save(OUT / f"state_{slug}.png")
    cw, ch = SIZE + 48, SIZE + 78
    sheet = Image.new("RGB", (cw * 4, ch * 2), (16, 18, 22))
    d = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(states):
        x0, y0 = (i % 4) * cw + 12, (i // 4) * ch + 12
        b = bezel(im)
        sheet.paste(b, (x0, y0))
        d.text((x0 + b.width // 2, y0 + b.height + 18), name[2:], font=font(20), fill=(230, 236, 245), anchor="mm")
    sheet.save(OUT / "standalone_screens_preview.png")
    print("saved", OUT / "standalone_screens_preview.png")


if __name__ == "__main__":
    main()
