# Renders the item and enemy art from gen_sprites.py into docs/img/*.png
# for docs/PLAYER_GUIDE.md. Run from the repo root.
import os, sys, tempfile
from PIL import Image

tmp = tempfile.mkdtemp()
sys.argv = ["gen_sprites.py", os.path.join(tmp, "s.h"), os.path.join(tmp, "s.png")]
g = {"__name__": "gen_sprites"}
exec(open("tools/gen_sprites.py").read(), g)

SH = [(224, 248, 208), (136, 192, 112), (52, 104, 86), (8, 24, 32)]   # DMG green
BG = SH[3]
S, PAD = 8, 2
os.makedirs("docs/img", exist_ok=True)
for name, cv, _ in (a for a in g["arts"] if a[0].endswith("2")):   # near tier
    white = name[:5] not in ("grunt", "bat0", "bat1", "bat2")
    w, h = len(cv[0]), len(cv)
    img = Image.new("RGB", (w + PAD * 2, h + PAD * 2), BG if white else SH[1])
    for y, row in enumerate(cv):
        for x, ch in enumerate(row):
            if ch != ".":
                v = g["VAL"][ch]
                if white:                   # OBP1: 1 light, 2 dark, 3 white
                    v = {1: 1, 2: 2, 3: 0}[v]
                img.putpixel((x + PAD, y + PAD), SH[v])
    img.resize((img.width * S, img.height * S), Image.NEAREST).save("docs/img/%s.png" % name)
print("ok", len(g["arts"]))
