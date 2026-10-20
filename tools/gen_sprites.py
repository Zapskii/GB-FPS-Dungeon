# Generates src/sprites.h + sprites_preview.png: billboard art for enemies
# and items, in 8x16 OBJ pairs.
#   '.' clear, 1 light, 2 dark, 3 black (OBP0: enemies)
#   '.' clear, 1 light, 2 dark, W white (OBP1: items, colour 3 shows white)
# Item tiers: tiny = NORMAL halved (or TINY), big = NORMAL doubled; enemies
# are drawn per tier. Pairs that repeat, or mirror an earlier pair, are stored once
# (mirrors use X-flip, flagged in bit 0 of the tile index).
import sys, zlib, struct
OUT_H = sys.argv[1] if len(sys.argv) > 1 else "src/sprites.h"
OUT_PNG = sys.argv[2] if len(sys.argv) > 2 else "sprites_preview.png"

# free OBJ pairs in the shared tiles (main.c: 154 = map arrow; 160-183 HUD,
# 184-196 font only during text screens, 238-239 banner L/R, 244-254 frame)
PAIRS = [156, 158] + list(range(184, 238, 2)) + [240, 242]

GRUNT = [
[  # far 8x16
"........",
"........",
"........",
"...333..",
"..32223.",
"..2.....",
".32....3",
".3222223",
".3222223",
".3222223",
".3222223",
".3222223",
"..22222.",
"..32223.",
"..33233.",
"...333..",
],
[  # mid 16x16
"................",
"................",
"................",
".....3333333....",
"....332222233...",
"...322..22..23..",
"..3322..22..233.",
"..3222222222223.",
"..3222222222223.",
"..3222222222223.",
"..3222222222223.",
"..3222222222223.",
"...32222222223..",
"...33222222233..",
"....333222333...",
"......33333.....",
],
[  # near 24x32
"........................",
"........................",
"........................",
"........................",
"........................",
".........3333333........",
".......33333333333......",
"......3333222223333.....",
".....333222222222333....",
".....332222222222233....",
"....33222..2222..2233...",
"....33222..2222..2233...",
"...3322222222222222233..",
"...3322222222222222233..",
"...3322222222222222233..",
"...3222222222222222223..",
"..332222222222222222233.",
"..332222222222222222233.",
"..332222222222222222233.",
"..332222222222222222233.",
"...3222222222222222223..",
"...3322222222222222233..",
"...3322222222222222233..",
"...3322222222222222233..",
"....33222222222222233...",
"....33222222222222233...",
".....332222222222233....",
".....333222222222333....",
"......3333222223333.....",
".......33333333333......",
".........3333333........",
"............3...........",
],
]

# bat: light wings, black body, light eyes; left halves, mirrored
def sym(left):
    return [r + r[::-1] for r in left]

BAT_FAR = [
"3.",
"13",
".3",
]
BAT_MID = sym([
"3...",
"33.3",
"3133",
"3113",
"3.31",
"...3",
])
BAT_NEAR = sym([
"3.......",
"33....3.",
"313...33",
"3113.333",
"31113313",
"31111333",
"3111.333",
"313.3.33",
"3....3.3",
".......3",
])

# items: NORMAL art (<= 8 wide, <= 16 tall); stairs keeps the old arrow.
# TINY overrides the halved art where halving loses the shape.
TINY = {
"key": [
".W...",
"W.WWW",
".W.W.",
],
"health": [
"W.W",
"WWW",
".W.",
],
"gold": [
".W.",
"W2W",
".W.",
],
"map": [
"WWWW",
"W22W",
"WWWW",
],
}
ITEMS = [
("key", [
".WWW.",
"W...W",
".WWW.",
"..W..",
"..WWW",
"..W..",
"..WWW",
]),
("health", [
".WW.WW.",
"WWWWWWW",
"WWWWWWW",
".WWWWW.",
"..WWW..",
"...W...",
]),
("gold", [
".WWWWW.",
"W2W2W2W",
"WWWWWWW",
".W2W2W.",
"..WWW..",
"...W...",
]),
("map", [
"WWWWWWW",
".W222W.",
".WWWWW.",
".W222W.",
".WWWWW.",
"WWWWWWW",
]),
("stairs", ["...WW..."] * 10 + [
"WWWWWWWW",
".WWWWWW.",
"..WWWW..",
"...WW...",
]),
]

VAL = {".": 0, "1": 1, "2": 2, "3": 3, "W": 3}
RANK = {".": 0, "1": 1, "2": 2, "3": 3, "W": 3}

def double(art):
    return [r for r in ("".join(c * 2 for c in row) for row in art) for _ in range(2)]

def halve(art):
    """2x2 blocks -> brightest-priority pixel (W/3 wins), keeps tiny icons solid."""
    h, w = len(art), len(art[0])
    out = []
    for y in range(0, h, 2):
        row = ""
        for x in range(0, w, 2):
            blk = [art[yy][xx] for yy in (y, y + 1) if yy < h for xx in (x, x + 1) if xx < w]
            row += max(blk, key=lambda c: RANK[c])
        out.append(row)
    return out

def place(art, cw, ch, valign):
    """Centre art horizontally in a cw x ch canvas; valign 'bottom' or 'mid'."""
    h, w = len(art), len(art[0])
    assert w <= cw and h <= ch, (w, h, cw, ch)
    x0 = (cw - w) // 2
    y0 = ch - h if valign == "bottom" else (ch - h) // 2
    c = [["."] * cw for _ in range(ch)]
    for y, row in enumerate(art):
        for x, v in enumerate(row):
            c[y0 + y][x0 + x] = v
    return ["".join(r) for r in c]

def fit(art, valign):
    """Smallest canvas of 8x16 sprites holding art."""
    w = (len(art[0]) + 7) // 8 * 8
    h = (len(art) + 15) // 16 * 16
    return place(art, w, h, valign)

store = []                                  # unique pairs, 32 bytes each
def pair_bytes(cv, sx, sy):
    data = []
    for y in range(16):
        lo = hi = 0
        for i, ch in enumerate(cv[sy * 16 + y][sx * 8:sx * 8 + 8]):
            v = VAL[ch]
            lo |= (v & 1) << (7 - i)
            hi |= (v >> 1) << (7 - i)
        data += [lo, hi]
    return data

def mirror(data):
    return [int("{:08b}".format(b)[::-1], 2) for b in data]

def tile_of(data):
    for n, d in enumerate(store):
        if d == data:
            return PAIRS[n]
        if mirror(d) == data:
            return PAIRS[n] | 1             # X-flip
    store.append(data)
    assert len(store) <= len(PAIRS), "out of OBJ tiles"
    return PAIRS[len(store) - 1]

def art_desc(cv):
    w, h = len(cv[0]) // 8, len(cv) // 16
    return [w, h] + [tile_of(pair_bytes(cv, sx, sy)) for sy in range(h) for sx in range(w)]

arts = []                                   # (C name, canvas, desc)
def add(name, cv):
    arts.append((name, cv, art_desc(cv)))

for t, cv in enumerate(GRUNT):
    add("grunt%d" % t, cv)
add("bat0", fit(sym(BAT_FAR), "mid"))
add("bat1", fit(BAT_MID, "mid"))
add("bat2", fit(BAT_NEAR, "mid"))
for name, art in ITEMS:
    add(name + "0", fit(TINY.get(name) or halve(art), "bottom"))
    add(name + "1", fit(art, "bottom"))
    add(name + "2", fit(double(art), "bottom"))

# ---------- sprites.h ----------
names = [a[0] for a in arts]
def runs():
    """(first tile, count, byte offset) per contiguous run of used pairs."""
    out, n = [], 0
    while n < len(store):
        m = n
        while m + 1 < len(store) and PAIRS[m + 1] == PAIRS[m] + 2:
            m += 1
        out.append((PAIRS[n], (m - n + 1) * 2, n * 32))
        n = m + 1
    return out

lines = []
for n, d in enumerate(store):
    lines.append("    /* %3d */ " % PAIRS[n] + ",".join("0x%02X" % b for b in d) + ",")
descs = []
for name, cv, d in arts:
    descs.append("static const uint8_t art_%s[] = { %s };" % (name, ", ".join("%du" % v for v in d)))
loads = "\n".join("    set_sprite_data(%du, %du, spr_tiles + %du);" % r for r in runs())
items = ", ".join("{ art_%s0, art_%s1, art_%s2 }" % (n, n, n) for n, _ in ITEMS)

h = f"""/* Billboard art for enemies and items: 8x16 OBJ pairs, {len(store)} unique
 * (tiles {", ".join("%d-%d" % (a, a + c - 1) for a, c, _ in runs())}).
 * Generated by tools/gen_sprites.py - edit the art there.
 * Art: {{w, h, tile...}} for w x h sprites, row-major; tile bit 0 = X-flip
 * (8x16 mode ignores bit 0 of the tile index). Tier 0 far .. 2 near.
 */
#ifndef SPRITES_H
#define SPRITES_H

#include <gb/gb.h>
#include <stdint.h>

static const uint8_t spr_tiles[] = {{
{chr(10).join(lines)}
}};

{chr(10).join(descs)}

/* [type][tier]: 0 grunt, 1 bat */
static const uint8_t * const enemy_art[2][3] = {{
    {{ art_grunt0, art_grunt1, art_grunt2 }}, {{ art_bat0, art_bat1, art_bat2 }}
}};
/* [kind][tier]: {", ".join("%d %s" % (i, n) for i, (n, _) in enumerate(ITEMS))} */
static const uint8_t * const item_art[{len(ITEMS)}][3] = {{
    {items}
}};

static void sprites_init(void)
{{
{loads}
}}

#endif
"""
open(OUT_H, "w").write(h)

# ---------- preview: every art on mid grey, 4x ----------
SH = [(255, 255, 255), (170, 170, 170), (85, 85, 85), (0, 0, 0)]
BG = (120, 120, 140)
S, GAP = 4, 4
cvs = [a[1] for a in arts]
PW = sum(len(c[0]) for c in cvs) + GAP * (len(cvs) + 1)
PH = max(len(c) for c in cvs) + GAP * 2
img = [[BG] * PW for _ in range(PH)]
x0 = GAP
for name, cv, _ in arts:
    white = name[:5] not in ("grunt", "bat0", "bat1", "bat2")
    for y, row in enumerate(cv):
        for x, ch in enumerate(row):
            if ch != ".":
                v = VAL[ch]
                if white:                   # OBP1 0x24: 1 light, 2 dark, 3 white
                    v = {1: 1, 2: 2, 3: 0}[v]
                img[PH - GAP - len(cv) + y][x0 + x] = SH[v]
    x0 += len(cv[0]) + GAP
raw = b"".join(b"\0" + b"".join(bytes(p) * S for p in r) for r in img for _ in range(S))
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", PW * S, PH * S, 8, 2, 0, 0, 0)) \
      + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
open(OUT_PNG, "wb").write(png)
print("ok", len(store), "pairs:", ", ".join("%s %s" % (n, d[2:]) for n, _, d in arts))
