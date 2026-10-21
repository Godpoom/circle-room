"""Draws the Circle Room mark and writes res/circleroom.ico + res/circleroom_256.png.
The mark is a circle inside a room: a graphite tile (the room), a paper-white ring (a lens /
a porthole) and a safelight-red disc resting low inside it, like a sun setting in a window.
Run from the project root: python tools/make_logo.py"""
import os
from PIL import Image, ImageDraw, ImageFilter

S = 1024
BG, EDGE, PAPER, RED, RED_HI = (28, 27, 25), (60, 57, 52), (234, 229, 219), (226, 82, 54), (255, 150, 110)


def mark():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, S - 1, S - 1), radius=int(S * 0.22), fill=BG)
    d.rounded_rectangle((8, 8, S - 9, S - 9), radius=int(S * 0.21), outline=EDGE, width=6)
    cx = cy = S // 2
    R = int(S * 0.31)       # ring outer radius
    w = int(S * 0.075)      # ring width
    # glow
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((cx - R * 1.25, cy - R * 1.25, cx + R * 1.25, cy + R * 1.25), fill=RED + (60,))
    im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(S * 0.05)))
    d = ImageDraw.Draw(im)
    # red disc sitting low inside the ring, clipped by the ring's inner edge
    disc = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    r = int(R * 0.62)
    dx, dy = cx + int(R * 0.06), cy + int(R * 0.22)
    ImageDraw.Draw(disc).ellipse((dx - r, dy - r, dx + r, dy + r), fill=RED + (255,))
    hi = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(hi).ellipse((dx - r * 0.55, dy - r * 0.75, dx + r * 0.25, dy - r * 0.05), fill=RED_HI + (190,))
    hi = hi.filter(ImageFilter.GaussianBlur(S * 0.03))
    disc_a = disc.split()[3]
    hi.putalpha(Image.composite(hi.split()[3], Image.new("L", (S, S), 0), disc_a))
    disc.alpha_composite(hi)
    inner = Image.new("L", (S, S), 0)
    ImageDraw.Draw(inner).ellipse((cx - (R - w), cy - (R - w), cx + (R - w), cy + (R - w)), fill=255)
    disc.putalpha(Image.composite(disc.split()[3], Image.new("L", (S, S), 0), inner))
    im.alpha_composite(disc)
    d = ImageDraw.Draw(im)
    # paper ring
    d.ellipse((cx - R, cy - R, cx + R, cy + R), outline=PAPER, width=w)
    return im


if __name__ == "__main__":
    os.makedirs("res", exist_ok=True)
    m = mark()
    m.resize((256, 256), Image.LANCZOS).save("res/circleroom_256.png")
    m.save("res/circleroom.ico", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    print("ok")
