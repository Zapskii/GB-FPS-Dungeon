"""Bench: build a scratch copy of src/ with bench.inc, render fixed views in
PyBoy, report mean render/sprite T and dump outputs for A/B comparison.
usage: bench.py SRC_DIR OUT.pkl [N] [--motion [K]]   |   bench.py --cmp A.pkl B.pkl
--motion: after each random view, K (default 8) follow-on images from that
pose (still / turn +-4, +-8 / walk 48, 64 / strafe 48 per image), so images
two apart (same bank) share columns (C5b skip); dumps also get r_wt, r_clr."""
import sys, os, re, shutil, subprocess, pickle, random
HERE = os.path.dirname(os.path.abspath(__file__))

def build(src, extra=()):
    w = os.path.join(HERE, 'bw'); shutil.rmtree(w, ignore_errors=True)
    shutil.copytree(src, os.path.join(w, 'src'))
    m = os.path.join(w, 'src', 'main.c'); s = open(m).read()
    s = s.replace('void main(void)\n{', 'void game_main(void)\n{', 1)
    s += '\n#include "../../bench.inc"\n'
    open(m, 'w').write(s)
    if '_cw_seg::' in open(os.path.join(w, 'src', 'render.s')).read(): extra = tuple(extra) + ('-DHAVE_SEG',)
    if '_c_key::' in open(os.path.join(w, 'src', 'render.s')).read(): extra = tuple(extra) + ('-DHAVE_KEY',)
    cmd = ['docker', 'run', '--rm', '-u', '%d:%d' % (os.getuid(), os.getgid()), '-v', HERE + ':/s', '-w', '/s/bw',
           'gbdk-dev', '/opt/gbdk/bin/lcc', '-Wl-m', '-Wl-j', *extra, '-o', 'b.gb',
           'src/main.c', 'src/isr.c', 'src/render.s', 'src/math.s', 'src/billboard.s'] + (['src/rtabs.s'] if os.path.exists(os.path.join(w, 'src', 'rtabs.s')) else [])
    for _ in range(4):
        __import__('time').sleep(0.5)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if os.path.exists(os.path.join(w, "b.gb")): break
    if not os.path.exists(os.path.join(w, 'b.gb')):
        print(r.stdout, r.stderr); sys.exit(1)
    sym = {}
    for l in open(os.path.join(w, 'b.noi')):
        p = l.split()
        if len(p) == 3 and p[0] == 'DEF': sym[p[1]] = int(p[2], 16)
    return os.path.join(w, 'b.gb'), sym

# motion kinds: (name, cam_a step, forward, sideways units per image)
MOTIONS = [('still', 0, 0, 0), ('turn+4', 4, 0, 0), ('turn-4', -4, 0, 0), ('turn+8', 8, 0, 0),
           ('turn-8', -8, 0, 0), ('walk48', 0, 48, 0), ('walk64', 0, 64, 0), ('strafe48', 0, 0, 48)]

def views(n, motion=0):
    rnd = random.Random(1234)
    mrnd = random.Random(5678)          # separate: the random views stay the default ones
    out = []
    for i in range(n):
        if i % 50 == 0:
            out.append(('L', i // 50 % 4, rnd.randrange(65536)))
        out.append(('V', rnd.random(), rnd.random(), rnd.random(), rnd.random(), rnd.randrange(256)))
        if motion:
            m = mrnd.choice(MOTIONS)
            if m[3] and mrnd.random() < 0.5: m = (m[0] + 'L', 0, 0, -m[3])
            out += [('M',) + m] * motion
    return out

def trig(src):
    """cos8/sin8 (int8, 127 = 1, cam_a 0..255 = full turn, 0 = +x) from tables.h"""
    t = open(os.path.join(src, 'tables.h')).read()
    return [[int(v) for v in re.search(nm + r'\[256\] = \{([^}]*)\}', t).group(1).replace(',', ' ').split()]
            for nm in ('cos8', 'sin8')]

def run(src, n, motion=0):
    from pyboy import PyBoy
    rom, S = build(src)
    cos8, sin8 = trig(src)
    pb = PyBoy(rom, window='null', sound_emulated=False)
    pb.set_emulation_speed(0)
    M = pb.memory
    def w8(a, v): M[a] = v & 0xFF
    def w16(a, v): w8(a, v); w8(a + 1, v >> 8)
    def rd(a, k): return bytes(M[a:a + k])
    def r32(a): return int.from_bytes(rd(a, 4), 'little')
    for _ in range(300): pb.tick(1, False)
    res, tr, ts, tc = [], [], [], []
    badc, views_seen = [0], [0]
    imap = None
    pose = None
    for v in views(n, motion):
        if v[0] == 'L':
            w8(S['_b_lvl'], v[1]); w16(S['_b_seed'], v[2]); w16(S['_b_cx'], 0xFFFF)
        elif v[0] == 'V':
            open_ = [i for i in range(1024) if imap[i] == 0]
            c = open_[int(v[1] * len(open_))]
            w8(S['_b_lvl'], 0xFF)
            w16(S['_b_cx'], ((c & 31) << 8) + 16 + int(v[3] * 224))
            w16(S['_b_cy'], ((c >> 5) << 8) + 16 + int(v[4] * 224))
            w8(S['_b_ca'], v[5])
            pose = [M[S['_b_cx']] | M[S['_b_cx'] + 1] << 8, M[S['_b_cy']] | M[S['_b_cy'] + 1] << 8, v[5]]
        if v[0] == 'M':
            x, y, a = pose
            a = (a + v[2]) & 255
            b = (a + 64) & 255
            dx = (v[3] * cos8[a] + v[4] * cos8[b]) // 127
            dy = (v[3] * sin8[a] + v[4] * sin8[b]) // 127
            # like move(): x then y, each only into an open cell (no doors: imap is the level's)
            if imap[(y >> 8 << 5) + ((x + dx) >> 8)] == 0: x += dx
            if imap[((y + dy) >> 8 << 5) + (x >> 8)] == 0: y += dy
            pose = [x, y, a]
            w8(S['_b_lvl'], 0xFF); w16(S['_b_cx'], x); w16(S['_b_cy'], y); w8(S['_b_ca'], a)
        w8(S['_b_done'], 0); w8(S['_b_go'], 1)
        for _ in range(400):
            pb.tick(1, False)
            if M[S['_b_done']]: break
        else:
            print('timeout'); sys.exit(1)
        if v[0] == 'L': imap = rd(S['_imap'], 1024)
        else:
            tr.append(r32(S['_b_t_r'])); ts.append(r32(S['_b_t_s'])); tc.append(int.from_bytes(rd(S['_b_t_c'], 2), 'little'))
            views_seen[0] += 1
        vram = rd(0x9000 if M[S['_b_bank']] else 0x8000, 2048) if v[0] != 'L' else b''
        e = (rd(S['_vbuf'], 2048), rd(S['_r_top'], 64), rd(S['_r_bot'], 64), rd(S['_r_zb'], 64),
             bytes(b for k in range(64) for b in ((int.from_bytes(rd(S['_r_hits'] + 2 * k, 2), 'little') - S['_imap']) & 0xFFFF).to_bytes(2, 'little')), rd(S['_seen'], 1024), rd(0xC000, 160), vram)
        if motion: e += (rd(S['_r_wt'], 64), rd(S['_r_clr'], 64), v[1] if v[0] == 'M' else v[0])
        res.append(e)
    pb.stop(save=False)
    print('copy mean %.0f T' % (sum(tc) / len(tc)))
    return res, tr, ts

if __name__ == '__main__':
    if sys.argv[1] == '--cmp':
        a = pickle.load(open(sys.argv[2], 'rb'))[0]; b = pickle.load(open(sys.argv[3], 'rb'))[0]
        names = 'vbuf top bot zb hits seen oam'.split()
        bad = 0
        for i, (x, y) in enumerate(zip(a, b)):
            # vbuf core rows may be left to copy_view now: compare the two
            # copied banks against each other, not against vbuf
            d = [names[k] for k in range(1, 7) if x[k] != y[k]]
            if i >= 3 and y[7] and y[7] != x[7]: d.append('vram != ref vram')
            if d:
                bad += 1
                if bad <= 10: print('view', i, 'differs:', d)
        print('%d/%d views differ' % (bad, len(a)))
        sys.exit(1 if bad else 0)
    motion = 0
    if '--motion' in sys.argv:
        k = sys.argv.index('--motion'); motion = 8
        if k + 1 < len(sys.argv) and sys.argv[k + 1].isdigit(): motion = int(sys.argv.pop(k + 1))
        sys.argv.pop(k)
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    res, tr, ts = run(sys.argv[1], n, motion)
    if motion:
        import collections
        print('motion: %d random views x (1 + %d) = %d views; sequences (type count): %s' % (n, motion, len(tr),
              ', '.join('%s %d' % (t, c // motion) for t, c in sorted(collections.Counter(e[10] for e in res if e[10] not in 'LV').items()))))
    pickle.dump((res, tr, ts), open(sys.argv[2], 'wb'))
    print('views %d  render mean %.0f T (min %d max %d)  sprite mean %.0f T' % (
        len(tr), sum(tr) / len(tr), min(tr), max(tr), sum(ts) / len(ts)))
