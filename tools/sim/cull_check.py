"""Safety check for project()'s per-image box radius (pj_r, cull_r[] from
tools/gen_cull.py): over bench views, 300 random objects per view within 10
cells go through a Python model of project() (fwd/lat, FOV, 60 px, wall at
the column); none it calls visible may fall outside the pj_r box.
usage: cull_check.py [N]"""
import sys, os, re, random, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bench
from pyboy import PyBoy

rom, S = bench.build(os.path.join(HERE, '..', '..', 'src'))
tab = subprocess.run([sys.executable, '-I', os.path.join(HERE, '..', 'gen_cull.py')], capture_output=True, text=True).stdout
cull_r = [int(x.rstrip('u')) for x in re.search(r'\{(.*)\}', tab).group(1).split(', ')]
pb = PyBoy(rom, window='null', sound_emulated=False); pb.set_emulation_speed(0)
M = pb.memory
def w8(a, v): M[a] = v & 0xFF
def w16(a, v): w8(a, v); w8(a + 1, v >> 8)
def u16(a): return M[a] | M[a + 1] << 8
def s8(v): return v - 256 if v >= 128 else v
def s16(v): return v - 65536 if v >= 32768 else v
cos8 = [s8(M[S['_cos8'] + i]) for i in range(256)]
sin8 = [s8(M[S['_sin8'] + i]) for i in range(256)]
for _ in range(300): pb.tick(1, False)
rnd, imap, visible, lost = random.Random(5), None, 0, 0
for v in bench.views(int(sys.argv[1]) if len(sys.argv) > 1 else 500):
    if v[0] == 'L':
        w8(S['_b_lvl'], v[1]); w16(S['_b_seed'], v[2]); w16(S['_b_cx'], 0xFFFF)
    else:
        open_ = [i for i in range(1024) if imap[i] == 0]
        c = open_[int(v[1] * len(open_))]
        w8(S['_b_lvl'], 0xFF)
        w16(S['_b_cx'], ((c & 31) << 8) + 16 + int(v[3] * 224)); w16(S['_b_cy'], ((c >> 5) << 8) + 16 + int(v[4] * 224)); w8(S['_b_ca'], v[5])
    w8(S['_b_done'], 0); w8(S['_b_go'], 1)
    for _ in range(400):
        pb.tick(1, False)
        if M[S['_b_done']]: break
    if v[0] == 'L': imap = bytes(M[S['_imap']:S['_imap'] + 1024]); continue
    cx, cy, ca = u16(S['_cam_x']), u16(S['_cam_y']), M[S['_cam_a']]
    zb = [M[S['_r_zb'] + i] for i in range(64)]
    r = cull_r[max(M[S['_r_top'] + i] for i in range(64))]
    cs, sn = cos8[ca], sin8[ca]
    for _ in range(300):
        dx, dy = rnd.randrange(-2559, 2560), rnd.randrange(-2559, 2560)
        qx, qy = dx >> 4, dy >> 4
        fwd = (qx * cs + qy * sn) >> 3
        lat = (qy * cs - qx * sn) >> 3
        if fwd < 96 or fwd > 2560 or abs(lat) >= fwd: continue
        off = (abs(lat) << 2 & 0xFFFF) // (fwd >> 4)
        if off >= 61: continue
        px = 80 + (-off if lat < 0 else off)
        if fwd >> 5 > zb[(px - 16) >> 1]: continue
        visible += 1
        if not (-r <= dx >> 8 < r and -r <= dy >> 8 < r): lost += 1
pb.stop(save=False)
print('%d visible objects, %d outside the pj_r box' % (visible, lost))
sys.exit(1 if lost or not visible else 0)
