"""A/B enemy_tick() between two src dirs in the SM83 interpreter: same bench
views, same random enemy state (alive, stun, type, positions near the camera,
dmg_cd, hp >= 2 so game_over never runs), then compare the state it leaves.
usage: enemy_ab.py OLD_SRC NEW_SRC [N]"""
import sys, os, random, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench, sm83
from pyboy import PyBoy

def load(src):
    rom, S = bench.build(src, ('-debug',))
    pb = PyBoy(rom, window='null', sound_emulated=False)
    pb.set_emulation_speed(0)
    for _ in range(300): pb.tick(1, False)
    def sym(name):
        for k, v in S.items():
            if k == '_' + name or re.fullmatch(r'Fmain\$%s\$0_0\$0' % name, k): return v
        raise KeyError(name)
    return pb, S, sym

old, new = load(sys.argv[1]), load(sys.argv[2])
n = int(sys.argv[3]) if len(sys.argv) > 3 else 200
VARS = [('ex', 16), ('ey', 16), ('ehit', 8), ('hp', 1), ('dmg_cd', 1), ('flash_n', 1), ('flash_pal', 1),
        ('ai_dist', 1024), ('ai_sp', 1)]
imap, bad, views, contact = None, 0, 0, 0
for v in bench.views(n):
    for pb, S, sym in (old, new):
        M = pb.memory
        def w8(a, x): M[a] = x & 0xFF
        def w16(a, x): w8(a, x); w8(a + 1, x >> 8)
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
    if v[0] == 'L':
        imap = bytes(old[0].memory[old[1]['_imap']:old[1]['_imap'] + 1024]); continue
    out = []
    for pb, S, sym in (old, new):
        mem = bytearray(pb.memory[0:0x10000])
        r = random.Random(views)                 # same state for both builds
        cx = mem[sym('cam_x')] | mem[sym('cam_x') + 1] << 8
        cy = mem[sym('cam_y')] | mem[sym('cam_y') + 1] << 8
        for i in range(8):
            mem[sym('ealive') + i] = r.random() < 0.8
            mem[sym('ehit') + i] = r.choice([0, 0, 0, 1, 3])
            mem[sym('etype') + i] = r.randrange(3)
            if r.random() < 0.5:
                x = (cx + r.randrange(-320, 320)) & 0xFFFF; y = (cy + r.randrange(-320, 320)) & 0xFFFF
                mem[sym('ex') + 2 * i:sym('ex') + 2 * i + 2] = x.to_bytes(2, 'little')
                mem[sym('ey') + 2 * i:sym('ey') + 2 * i + 2] = y.to_bytes(2, 'little')
        mem[sym('dmg_cd')] = r.choice([0, 0, 5])
        mem[sym('hp')] = r.randrange(2, 6)
        hp0 = mem[sym('hp')]
        sm83.run(mem, S['Fmain$enemy_tick$0$0'])
        out.append([bytes(mem[sym(k):sym(k) + l]) for k, l in VARS])
    if out[0][3][0] != hp0: contact += 1
    d = [VARS[k][0] for k in range(len(VARS)) if out[0][k] != out[1][k]]
    if d:
        bad += 1
        if bad <= 10: print('view', views, 'differs:', d)
    views += 1
for pb, _, _ in (old, new): pb.stop(save=False)
print('%d/%d views differ (%d with contact damage)' % (bad, views, contact))
sys.exit(1 if bad else 0)
