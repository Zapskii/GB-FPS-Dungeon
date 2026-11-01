#!/usr/bin/env python3
"""Draw the 256x224 stone-frame SGB border art (input to GB-SGB-Tooling's
mkborder.py --art). Bricks are 16x8 and tile-aligned so the art dedupes to a
few dozen tiles. Usage: stone_art.py out.png"""
import sys
from PIL import Image

W, H = 256, 224
HX, HY, HW, HH = 48, 40, 160, 144          # Game Boy screen hole
MORTAR = (24, 22, 30)
STONES = [(88, 84, 96), (104, 100, 110), (76, 72, 84)]  # three brick shades
HI, LO = (136, 132, 144), (48, 44, 56)     # brick highlight / shadow
SPECK = (64, 60, 72)
RIM = [(16, 14, 20), (40, 36, 48), (120, 116, 128), (16, 14, 20)]  # bevel, outside in

img = Image.new("RGBA", (W, H))
px = img.load()

def h(a, b):                                # deterministic per-brick hash
    return (a * 73 + b * 151 + a * b * 7) & 0xFF

for y in range(H):
    row, by = y // 8, y % 8
    off = 8 if row & 1 else 0
    for x in range(W):
        bx_i, bx = (x + off) // 16, (x + off) % 16
        k = h(bx_i, row)
        c = STONES[k % 3]
        if by == 0 or bx == 0:
            c = MORTAR
        elif by == 1 or bx == 1:
            c = HI
        elif by == 7 or bx == 15:
            c = LO
        elif k & 8 and (bx * 5 + by * 3) % 11 == 0:
            c = SPECK
        px[x, y] = c + (255,)

# bevelled rim round the hole, 4 px wide, then the hole itself transparent
for d, c in enumerate(RIM):
    for x in range(HX - 4 + d, HX + HW + 4 - d):
        px[x, HY - 4 + d] = px[x, HY + HH + 3 - d] = c + (255,)
    for y in range(HY - 4 + d, HY + HH + 4 - d):
        px[HX - 4 + d, y] = px[HX + HW + 3 - d, y] = c + (255,)
for y in range(HY, HY + HH):
    for x in range(HX, HX + HW):
        px[x, y] = (0, 0, 0, 0)

img.save(sys.argv[1])
