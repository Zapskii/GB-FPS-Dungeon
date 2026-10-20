"""Bench: build a scratch copy of src/ with bench.inc, render fixed views in
PyBoy, report mean render/sprite T and dump outputs for A/B comparison.
usage: bench.py SRC_DIR OUT.pkl [N]    |   bench.py --cmp A.pkl B.pkl"""
import sys, os, shutil, subprocess, pickle, random
HERE = os.path.dirname(os.path.abspath(__file__))

def build(src, extra=()):
    w = os.path.join(HERE, 'bw'); shutil.rmtree(w, ignore_errors=True)
    shutil.copytree(src, os.path.join(w, 'src'))
    m = os.path.join(w, 'src', 'main.c'); s = open(m).read()
    s = s.replace('void main(void)\n{', 'void game_main(void)\n{', 1)
    s += '\n#include "../../bench.inc"\n'
    open(m, 'w').write(s)
    if '_cw_seg::' in open(os.path.join(w, 'src', 'render.s')).read(): extra = tuple(extra) + ('-DHAVE_SEG',)
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

def views(n):
    rnd = random.Random(1234)
    out = []
    for i in range(n):
        if i % 50 == 0:
            out.append(('L', i // 50 % 4, rnd.randrange(65536)))
        out.append(('V', rnd.random(), rnd.random(), rnd.random(), rnd.random(), rnd.randrange(256)))
    return out

def run(src, n):
    from pyboy import PyBoy
    rom, S = build(src)
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
    for v in views(n):
        if v[0] == 'L':
            w8(S['_b_lvl'], v[1]); w16(S['_b_seed'], v[2]); w16(S['_b_cx'], 0xFFFF)
        else:
            open_ = [i for i in range(1024) if imap[i] == 0]
            c = open_[int(v[1] * len(open_))]
            w8(S['_b_lvl'], 0xFF)
            w16(S['_b_cx'], ((c & 31) << 8) + 16 + int(v[3] * 224))
            w16(S['_b_cy'], ((c >> 5) << 8) + 16 + int(v[4] * 224))
            w8(S['_b_ca'], v[5])
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
        res.append((rd(S['_vbuf'], 2048), rd(S['_r_top'], 64), rd(S['_r_bot'], 64), rd(S['_r_zb'], 64),
                    bytes(b for k in range(64) for b in ((int.from_bytes(rd(S['_r_hits'] + 2 * k, 2), 'little') - S['_imap']) & 0xFFFF).to_bytes(2, 'little')), rd(S['_seen'], 1024), rd(0xC000, 160), vram))
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
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    res, tr, ts = run(sys.argv[1], n)
    pickle.dump((res, tr, ts), open(sys.argv[2], 'wb'))
    print('views %d  render mean %.0f T (min %d max %d)  sprite mean %.0f T' % (
        len(tr), sum(tr) / len(tr), min(tr), max(tr), sum(ts) / len(ts)))
