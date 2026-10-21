"""Draws the Darkroom mark and writes res/darkroom.ico + res/darkroom_256.png.
The mark is a "D": a paper-white bar (the film strip / enlarger post) and a half disc in
safelight red (the lamp). Run: python tools/make_logo.py"""
import math, os
from PIL import Image, ImageDraw, ImageFilter

S = 1024
BG, PAPER, RED, RED_HI = (28, 27, 25), (234, 229, 219), (226, 82, 54), (255, 150, 110)

def mark():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, S - 1, S - 1), radius=int(S * 0.22), fill=BG)
    d.rounded_rectangle((8, 8, S - 9, S - 9), radius=int(S * 0.21), outline=(60, 57, 52), width=6)
    # glow behind the lamp
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    cx, cy, r = int(S * 0.47), int(S * 0.5), int(S * 0.25)
    g.ellipse((cx - r * 1.5, cy - r * 1.5, cx + r * 1.5, cy + r * 1.5), fill=RED + (70,))
    glow = glow.filter(ImageFilter.GaussianBlur(S * 0.05))
    im.alpha_composite(glow)
    d = ImageDraw.Draw(im)
    # bar
    bx0, bx1, by0, by1 = int(S * 0.27), int(S * 0.39), int(S * 0.25), int(S * 0.75)
    d.rectangle((bx0, by0, bx1, by1), fill=PAPER)
    # lamp: half disc to the right of the bar, with a radial gradient
    lamp = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lamp)
    R = int(S * 0.25)
    ccx = bx1 + int(S * 0.03)
    ld.pieslice((ccx - R, cy - R, ccx + R, cy + R), -90, 90, fill=RED + (255,))
    ld.rectangle((ccx, cy - R, ccx + int(S * 0.005), cy + R), fill=RED + (255,))
    hi = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hi)
    hd.ellipse((ccx + R * 0.05, cy - R * 0.55, ccx + R * 0.65, cy + R * 0.05), fill=RED_HI + (170,))
    hi = hi.filter(ImageFilter.GaussianBlur(S * 0.035))
    mask = lamp.split()[3]
    hi.putalpha(Image.composite(hi.split()[3], Image.new("L", (S, S), 0), mask))
    lamp.alpha_composite(hi)
    im.alpha_composite(lamp)
    return im

os.makedirs("res", exist_ok=True)
m = mark()
m.resize((256, 256), Image.LANCZOS).save("res/darkroom_256.png")
sizes = [16, 24, 32, 48, 64, 128, 256]
m.save("res/darkroom.ico", sizes=[(s, s) for s in sizes])
print("ok")
