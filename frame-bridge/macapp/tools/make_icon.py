#!/usr/bin/env python3
"""Draw the Frame Bridge app icon (a visor outline under wireless arcs) and write a macOS .icns.
usage: make_icon.py OUT.icns [preview.png]"""
import io, struct, sys
from PIL import Image, ImageDraw, ImageFilter


def draw(size):
    s = 1024
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    # macOS icon grid: content square ~824 px with a continuous-corner rounded rect
    m, r = 100, 185
    bg = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    grad = Image.new("RGBA", (s, s))
    gd = ImageDraw.Draw(grad)
    for y in range(s):
        t = y / s
        gd.line([(0, y), (s, y)], fill=(int(24 + 30 * t), int(32 + 20 * t), int(58 + 40 * t), 255))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([m, m, s - m, s - m], radius=r, fill=255)
    bg.paste(grad, (0, 0), mask)
    # soft shadow under the tile
    sh = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle([m, m + 18, s - m, s - m + 18], radius=r, fill=(0, 0, 0, 110))
    img = Image.alpha_composite(img, sh.filter(ImageFilter.GaussianBlur(22)))
    img = Image.alpha_composite(img, bg)
    d = ImageDraw.Draw(img)
    accent = (110, 214, 255, 255)
    white = (240, 246, 255, 255)
    # wireless arcs
    cx, cy = s // 2, 470
    for i, rad in enumerate((90, 170, 250)):
        d.arc([cx - rad, cy - rad, cx + rad, cy + rad], 220, 320, fill=accent, width=46 - i * 4)
    d.ellipse([cx - 34, cy - 34, cx + 34, cy + 34], fill=accent)
    # visor
    vx0, vy0, vx1, vy1 = 250, 560, 774, 780
    d.rounded_rectangle([vx0, vy0, vx1, vy1], radius=105, outline=white, width=40)
    # nose bridge notch
    d.pieslice([cx - 70, vy1 - 80, cx + 70, vy1 + 60], 180, 360, fill=(34, 44, 80, 255))
    d.arc([cx - 70, vy1 - 80, cx + 70, vy1 + 60], 180, 360, fill=white, width=40)
    return img.resize((size, size), Image.LANCZOS)


def icns(path, preview=None):
    entries = [(b"ic07", 128), (b"ic08", 256), (b"ic09", 512), (b"ic10", 1024), (b"ic11", 32), (b"ic12", 64),
               (b"ic13", 256), (b"ic14", 512)]
    big = draw(1024)
    if preview:
        big.save(preview)
    chunks = b""
    for typ, px in entries:
        buf = io.BytesIO()
        big.resize((px, px), Image.LANCZOS).save(buf, "PNG")
        data = buf.getvalue()
        chunks += typ + struct.pack(">I", len(data) + 8) + data
    open(path, "wb").write(b"icns" + struct.pack(">I", len(chunks) + 8) + chunks)


if __name__ == "__main__":
    icns(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
