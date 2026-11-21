#!/usr/bin/env python3
"""Render 360x360 previews of the standalone Grok Sphere screens & emotional capsule-eye moods.
Geometry and colors are copied from firmware/main/ui.c."""
import math
from pathlib import Path
import qrcode
from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).resolve().parent
SIZE = 360
EYE_W, EYE_H, EYE_DX, EYE_CY = 74, 120, 62, 160
REPLY_W, REPLY_H, REPLY_Y = 232, 76, 248
C_IDLE, C_LISTEN, C_THINK = (0xEA, 0xF2, 0xFF), (0x3D, 0xDC, 0xFF), (0x9A, 0xA7, 0xFF)
C_SAD, C_SLEEPY, C_BORED = (0xA8, 0xC8, 0xFF), (0x9A, 0xA6, 0xBA), (0xCF, 0xD8, 0xE6)
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


def draw_rotated_rect(im, x, y, w, h, angle_deg, fill=(0, 0, 0)):
    patch = Image.new("RGBA", (w, h), fill + (255,))
    rot = patch.rotate(-angle_deg, expand=True, resample=Image.BICUBIC)
    rx = int(x + w / 2 - rot.width / 2)
    ry = int(y + h / 2 - rot.height / 2)
    im.paste(rot, (rx, ry), rot)


def draw_rotated_capsule(im, cx, cy, w, h, col, angle_deg):
    pad = 24
    patch = Image.new("RGBA", (w + pad * 2, h + pad * 2), (0, 0, 0, 0))
    pd = ImageDraw.Draw(patch)
    pd.rounded_rectangle((pad, pad, pad + w, pad + h), radius=w / 2, fill=col + (255,))
    if angle_deg:
        patch = patch.rotate(-angle_deg, expand=True, resample=Image.BICUBIC)
    im.paste(patch, (int(cx - patch.width / 2), int(cy - patch.height / 2)), patch)


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


def eyes(col, w=EYE_W, h=EYE_H, cy=EYE_CY, dots=False, status="", reply="", mood="none"):
    im = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
    d = ImageDraw.Draw(im)
    if mood == "happy":
        for i in range(2):
            cx = 180 + (-EYE_DX if i == 0 else EYE_DX)
            capsule(d, cx, cy - 6, 78, 86, col)
            d.rounded_rectangle((cx - 48, cy - 2, cx + 48, cy - 2 + 76), radius=48, fill=(0, 0, 0))
    elif mood == "sad":
        for i in range(2):
            cx = 180 + (-EYE_DX if i == 0 else EYE_DX)
            capsule(d, cx, cy + 8, 72, 88, col)
            lx = cx - 54 + (-8 if i == 0 else 8)
            ly = (cy + 8) - 56
            draw_rotated_rect(im, lx, ly, 108, 52, -20 if i == 0 else 20, (0, 0, 0))
    elif mood == "bored":
        gx = 10
        for i in range(2):
            cx = 180 + (-EYE_DX if i == 0 else EYE_DX) + gx
            capsule(d, cx, cy, 76, 106, col)
            d.rectangle((cx - 48, cy - 58, cx + 48, cy - 2), fill=(0, 0, 0))
    elif mood == "sleepy":
        for i in range(2):
            cx = 180 + (-EYE_DX if i == 0 else EYE_DX)
            capsule(d, cx, cy + 6, 76, 104, col)
            d.rectangle((cx - 48, (cy + 6) - 54, cx + 48, (cy + 6) - 54 + 84), fill=(0, 0, 0))
        d.text((250, 92), "z Z", font=font(20), fill=C_STATUS)
    elif mood == "surprised":
        for i in range(2):
            cx = 180 + (-EYE_DX if i == 0 else EYE_DX)
            capsule(d, cx, cy - 10, 88, 148, col)
    elif mood == "curious":
        draw_rotated_capsule(im, 180 - EYE_DX, cy - 8, 82, 134, col, -4)
        draw_rotated_capsule(im, 180 + EYE_DX, cy + 6, 68, 84, col, 4)
    elif dots:
        capsule(d, 180 - EYE_DX, cy - 22, 78, 66, col)
        capsule(d, 180 + EYE_DX, cy - 22, 68, 48, col)
    else:
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


def make_sheet(items, out_path):
    cw, ch = SIZE + 48, SIZE + 78
    sheet = Image.new("RGB", (cw * 4, ch * 2), (16, 18, 22))
    d = ImageDraw.Draw(sheet)
    for i, (label, im) in enumerate(items):
        x0, y0 = (i % 4) * cw + 12, (i // 4) * ch + 12
        b = bezel(im)
        sheet.paste(b, (x0, y0))
        d.text((x0 + b.width // 2, y0 + b.height + 18), label, font=font(20), fill=(230, 236, 245), anchor="mm")
    sheet.save(out_path)
    print("saved", out_path)


def main():
    states = [
        ("1 Setup hotspot", qr_screen("Sphere setup", "WIFI:T:WPA;S:GrokSphere-3A7C;P:48213907;;",
                                      "Scan with your phone camera, or join\nGrokSphere-3A7C  /  48213907\nthen open  http://192.168.4.1")),
        ("2 Key only (on home Wi-Fi)", qr_screen("Add your Claude key", "http://192.168.1.50/",
                                                 "Phone on the same Wi-Fi:\nscan, or open http://192.168.1.50\n(or http://grok-sphere.local)")),
        ("3 Idle", eyes(C_IDLE)),
        ("4 Listening (tap)", eyes(C_LISTEN, 84, 140, status="Listening...")),
        ("5 Thinking", eyes(C_THINK, EYE_W, 56, EYE_CY, dots=True, reply="What's a good name for a fantasy football team?")),
        ("6 Speaking + reply text", eyes(C_IDLE, EYE_W, 96, reply="How about the Fort Worth Fumbles? Self-aware, local, and honestly a bit of a warning.")),
        ("7 Error", eyes(C_ERR, mood="sad", status="Claude key rejected")),
        ("8 Clock + weather (hold)", clock()),
    ]
    for name, im in states:
        slug = "_".join(name.lower().replace("(", "").replace(")", "").replace("+", "").split()[1:])
        im.save(OUT / f"state_{slug}.png")
    make_sheet([(n[2:], im) for n, im in states], OUT / "standalone_screens_preview.png")

    moods = [
        ("Idle (neutral)",      eyes(C_IDLE)),
        ("Happy (reply)",       eyes(C_IDLE, mood="happy", status="Happy")),
        ("Curious (thinking)",  eyes(C_IDLE, mood="curious", status="Curious")),
        ("Surprised (wake)",    eyes(C_IDLE, mood="surprised", status="Surprised")),
        ("Sad (mood / error)",  eyes(C_SAD, mood="sad", status="Sad")),
        ("Bored (2 min idle)",  eyes(C_BORED, mood="bored", status="Bored")),
        ("Sleepy (10 min)",     eyes(C_SLEEPY, mood="sleepy", status="Sleepy")),
        ("Error (red + sad)",   eyes(C_ERR, mood="sad", status="Didn't catch that")),
    ]
    make_sheet(moods, OUT / "standalone_moods_preview.png")


if __name__ == "__main__":
    main()
