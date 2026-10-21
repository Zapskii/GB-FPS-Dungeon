"""Run copy_view under ppusim on bench snapshots. usage: copysim.py SRC [N]"""
import sys, os, random, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench, ppusim

src = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
rom, S = bench.build(src)
CK = S.get('_c_key'); BK = S.get('_b_key')   # C5b column keys (render.s) and their pre-copy backup (bench.inc -DHAVE_KEY)
if CK is not None and BK is None: print('_c_key but no _b_key (bench built without -DHAVE_KEY): c_key not restored')
SKIP = collections.Counter()
from pyboy import PyBoy
pb = PyBoy(rom, window='null', sound_emulated=False)
pb.set_emulation_speed(0)
M = pb.memory
def w8(a, v): M[a] = v & 0xFF
def w16(a, v): w8(a, v); w8(a + 1, v >> 8)
for _ in range(300): pb.tick(1, False)
rnd = random.Random(7)
imap = None
stats = collections.Counter()
minslack = 10 ** 9
tot_t = []
PCS = collections.Counter(); NV = [0]; PSL = {}; LATE = [0]; PCC = collections.Counter(); HALT = [0]
for v in bench.views(n):
    if v[0] == 'L':
        w8(S['_b_lvl'], v[1]); w16(S['_b_seed'], v[2]); w16(S['_b_cx'], 0xFFFF)
    else:
        op = [i for i in range(1024) if imap[i] == 0]; c = op[int(v[1] * len(op))]
        w8(S['_b_lvl'], 0xFF); w16(S['_b_cx'], ((c & 31) << 8) + 16 + int(v[3] * 224))
        w16(S['_b_cy'], ((c >> 5) << 8) + 16 + int(v[4] * 224)); w8(S['_b_ca'], v[5])
    w8(S['_b_done'], 0); w8(S['_b_go'], 1)
    for _ in range(400):
        pb.tick(1, False)
        if M[S['_b_done']]: break
    if v[0] == 'L':
        imap = bytes(M[S['_imap']:S['_imap'] + 1024]); continue
    snap = bytearray(M[0:0x10000])
    # some sprites so the band logic is exercised: keep bench OAM, add a gun
    oam = snap[0xC000:0xC0A0]
    for i in range(6):
        if oam[4 * i] == 0:
            oam[4 * i] = 80 + 16 * (i // 3); oam[4 * i + 1] = 80 + 8 * (i % 3)
    snap[0xC000:0xC0A0] = oam
    ys = [oam[4 * i] for i in range(40) if 0 < oam[4 * i] < 160]
    for mode in ('full', 'spans'):
        for model in ('sprites', 'adversarial'):
            raw = bytearray(snap)
            raw[S['_r_vh']:S['_r_vh'] + 64] = raw[S['_b_vh']:S['_b_vh'] + 64]   # history before the bench's copy
            if mode == 'full':
                for k in range(32): raw[S['_r_vh'] + k] = 0; raw[S['_r_vh'] + 32 + k] = 64
            if BK is not None: raw[CK:CK + 512] = raw[BK:BK + 512]   # keys before the bench's copy
            if CK is not None and mode == 'full':
                for k in range(32): raw[CK + 16 * k] = 0xFF               # view_reset invalidates them
            if CK is not None:                                            # c_desc: undo the bench copy's skips
                CD = S['_seen'] + 1024
                for c in range(16):
                    d = CD + 8 * c
                    if raw[d] == raw[d + 3]:
                        raw[d + 1], raw[d + 2] = raw[d + 6], raw[d + 7]
                        raw[d + 6] = min(raw[S['_r_top'] + 4 * c:S['_r_top'] + 4 * c + 4]); raw[d + 7] = max(raw[S['_r_bot'] + 4 * c:S['_r_bot'] + 4 * c + 4])
            raw[0xFFFF] = 0x01
            l3 = []
            lo = min(ys) - 16 if ys else 999
            hi = max(ys) - 1 if ys else -1
            for y in range(144):
                cnt = min(10, sum(1 for oy in ys if oy - 16 <= y < oy))
                win = 6 if y >= 128 else 0
                if model == 'sprites':
                    l3.append(172 + win + 11 * cnt + rnd.randrange(0, 3))
                else:
                    l3.append(rnd.randrange(172, 290) if lo <= y <= hi else 172 + win + rnd.randrange(0, 3))
            dst = raw[S['_r_dst']] | raw[S['_r_dst'] + 1] << 8
            ref = bytes(raw[dst:dst + 2048])       # the bench's copy of this image
            if CK is not None: kb = CK + (256 if dst >= 0x9000 else 0); k0 = bytes(raw[kb:kb + 256])
            raw[dst:dst + 2048] = bytes(2048) if mode == 'full' else raw[dst:dst + 2048]
            sim = ppusim.Sim(raw, l3, t0=rnd.randrange(154 * 456), isr_t=rnd.randrange(2500, 4500))
            try:
                t = sim.run(S['_copy_view'])
            except RuntimeError as e:
                print('ERROR', e); sys.exit(1)
            if sim.viol:
                stats['viol_' + model] += len(sim.viol)
                for vv in sim.viol[:3]: print(model, mode, 'viol t=%d line %d x %d pc %04X L3 %d prevL3 %d' % vv)
            if bytes(raw[dst:dst + 2048]) != ref: stats[mode + '_mismatch'] += 1
            for lt, lpc in sim.late:
                stats['late_halts'] += 1
                if lt > LATE[0]: LATE[0] = lt; print('latest halt %d T late at %04X (%s %s)' % (lt, lpc, model, mode))
            if model == 'sprites': tot_t.append((mode, t, sim.writes))
            if CK is not None and model == 'sprites':   # the skip rule: this bank's valid key == the column's 16 ray bytes
                SKIP[mode] += sum(k0[16 * c] != 0xFF and k0[16 * c:16 * c + 16] == b''.join(
                    raw[S[a] + 4 * c:S[a] + 4 * c + 4] for a in ('_r_top', '_r_bot', '_r_wt', '_r_clr')) for c in range(16))
            if model == 'sprites' and mode == 'spans': PCS.update(sim.pcs); PCC.update(sim.pcc); NV[0] += 1; HALT[0] += t - sum(sim.pcc.values())
            for k, vv in sim.pcslack.items(): PSL[k] = min(PSL.get(k, 10 ** 9), vv)
            if sim.slack < minslack: minslack = sim.slack; print('new min slack', sim.slack, model, mode, 't=%d line %d x %d pc %04X L3 %d' % sim.slack_at)
            stats['runs'] += 1
pb.stop(save=False)
print(dict(stats), 'min slack', minslack)
for mode in ('full', 'spans'):
    xs = [(t, w) for m, t, w in tot_t if m == mode]
    print('%-5s copy %.0f T = %.0f lines, %.0f bytes, %.2f bytes/line' % (
        mode, sum(t for t, w in xs) / len(xs), sum(t for t, w in xs) / len(xs) / 456,
        sum(w for t, w in xs) / len(xs), sum(w for t, w in xs) / sum(t for t, w in xs) * 456)
        + (', %.2f cols skipped' % (SKIP[mode] / len(xs)) if CK is not None else ''))

import re, subprocess
w = os.path.join(bench.HERE, 'bw', 'src')
subprocess.run(['docker', 'run', '--rm', '-u', '%d:%d' % (os.getuid(), os.getgid()), '-v', bench.HERE + ':/w',
                '-w', '/w/bw/src', 'gbdk-dev', '/opt/gbdk/bin/sdasgb', '-plosgff', 'render.rel', 'render.s'], capture_output=True)
area = None; anchor = None; labs = {}
for l in open(os.path.join(w, 'render.lst')):
    a = re.search(r'\.area\s+(\w+)', l)
    if a: area = a.group(1)
    lab = re.match(r'^.{4}([0-9A-F]{8})\s+\d+\s+([A-Za-z_]\w*)::?', l)
    if lab and area == '_CODE':
        if anchor is None and lab.group(2).startswith('_'): anchor = (lab.group(2), int(lab.group(1), 16))
        labs[int(lab.group(1), 16)] = lab.group(2)
base = S[anchor[0]] - anchor[1]
for off, name in sorted(labs.items()):
    if name[:3] in ('cw_', 'cv_', 'cu_'):
        print('  %-10s %7.1f per copy' % (name, PCS[base + off] / NV[0]))

import bisect as _b
_offs = sorted(labs); _bases = [base + o for o in _offs]
bylab = collections.Counter()
for pc, cyc in PCC.items():
    i = _b.bisect_right(_bases, pc) - 1
    bylab[labs[_offs[i]] if i >= 0 else '?'] += cyc
print('T per copy by label (+ halted %.0f):' % (HALT[0] / NV[0]))
for k, cyc in bylab.most_common(30):
    print('  %-10s %7.0f T' % (k, cyc / NV[0]))
lab_of = {}
cur = None
for off in sorted(labs):
    pass
import bisect
offs = sorted(labs)
print('min slack per store site (halt bursts and others):')
for pc, sl in sorted(PSL.items(), key=lambda x: x[1])[:12]:
    i = bisect.bisect_right([base + o for o in offs], pc) - 1
    print('  %04X  after %-10s slack %d' % (pc, labs[offs[i]], sl))
