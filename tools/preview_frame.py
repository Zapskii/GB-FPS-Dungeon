# Decodes frame_tiles from frame.h + mirrors frame_draw() layout into a 160x128 PNG (scaled 3x).
import re, zlib, struct, sys
src = open(sys.argv[1]).read()
body = src.split('frame_tiles[11u * 16u] = {')[1].split('};')[0]
data = [int(v, 16) for v in re.findall(r'0x([0-9A-F]{2})u', body)]
assert len(data) == 176
BR, TOP, TOPR, BOT, BOTR, LF, RT, TL, TR, BL, BRc = range(11)
m = [[BR]*20 for _ in range(16)]
for y in range(4, 12): m[y][1] = LF; m[y][18] = RT; m[y][2:18] = [-1]*16
m[3][1:19] = [TL] + [TOP]*16 + [TR]; m[3][6] = m[3][13] = TOPR
m[12][1:19] = [BL] + [BOT]*16 + [BRc]; m[12][6] = m[12][13] = BOTR
grey = [255, 170, 85, 0]
S = 3
rows = []
for py in range(128):
    line = bytearray()
    for px in range(160):
        t = m[py // 8][px // 8]
        if t < 0: g = 128
        else:
            lo, hi = data[t*16 + (py % 8)*2], data[t*16 + (py % 8)*2 + 1]
            b = 7 - px % 8
            g = grey[((hi >> b) & 1) << 1 | ((lo >> b) & 1)]
        line += bytes([g]) * S
    rows += [line] * S
raw = b''.join(b'\0' + bytes(r) for r in rows)
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 160*S, 128*S, 8, 0, 0, 0, 0)) \
      + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
