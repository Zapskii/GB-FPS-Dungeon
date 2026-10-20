"""Exact cycle profile of an asm routine over bench views (SM83 interpreter).
usage: prof.py SRC_DIR [N] [ENTRY]   ENTRY = _render_asm by default"""
import sys, os, re, subprocess, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench, sm83

src = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
entry = sys.argv[3] if len(sys.argv) > 3 else '_render_asm'
rom, S = bench.build(src, ('-debug',) if os.environ.get('DEBUG') else ())
w = os.path.join(bench.HERE, 'bw', 'src')
labels = {}                                   # address -> (label, source line)
for mod in ('render', 'math'):
    subprocess.run(['docker', 'run', '--rm', '-u', '%d:%d' % (os.getuid(), os.getgid()), '-v', bench.HERE + ':/w',
                    '-w', '/w/bw/src', 'gbdk-dev', '/opt/gbdk/bin/sdasgb', '-plosgff', mod + '.rel', mod + '.s'],
                   capture_output=True)
    area, label, anchor, rows = None, '?', None, []
    for l in open(os.path.join(w, mod + '.lst')):
        a = re.search(r'\.area\s+(\w+)', l)
        if a: area = a.group(1)
        lab = re.match(r'^.{4}([0-9A-F]{8})\s+\d+\s+([A-Za-z_]\w*)::?', l)
        if lab and area == '_CODE':
            label = lab.group(2)
            if anchor is None and label.startswith('_'): anchor = (label, int(lab.group(1), 16))
        m = re.match(r'^.{4}([0-9A-F]{8}) ((?:[0-9A-F]{2}[ rsRSuU]?)+?)\s*\[\s*(\d+)\]\s+\d+\s+(.*)$', l)
        if m and area == '_CODE': rows.append((int(m.group(1), 16), label, m.group(4).split(';')[0].strip()))
    base = S[anchor[0]] - anchor[1]
    for off, lab, t in rows: labels[base + off] = (lab, t)

# C functions and lines from the debug .noi
import bisect
cfun, cline = [], []
for l in open(os.path.join(bench.HERE, 'bw', 'b.noi')):
    q = l.split()
    if len(q) != 3: continue
    a = int(q[2], 16)
    if a >= 0x8000: continue
    m = re.match(r'F(\w+)\$(\w+)\$', q[1])
    if m: cfun.append((a, m.group(2)))
    m = re.match(r'C\$([\w.]+)\$(\d+)\$', q[1])
    if m: cline.append((a, '%s:%s' % (m.group(1), m.group(2))))
    if q[1].startswith('_') and not q[1].startswith('__'): cfun.append((a, q[1]))
cfun.sort(); cline.sort()
cfa = [a for a, _ in cfun]; cla = [a for a, _ in cline]
def clab(a):
    i = bisect.bisect_right(cfa, a) - 1
    return cfun[i][1] if i >= 0 else '?'
def cln(a):
    i = bisect.bisect_right(cla, a) - 1
    return cline[i][1] if i >= 0 else '?'
from pyboy import PyBoy
pb = PyBoy(rom, window='null', sound_emulated=False)
pb.set_emulation_speed(0)
M = pb.memory
def w8(a, v): M[a] = v & 0xFF
def w16(a, v): w8(a, v); w8(a + 1, v >> 8)
for _ in range(300): pb.tick(1, False)
imap, views, tot = None, 0, 0
prof = collections.Counter()
for v in bench.views(n):
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
        imap = bytes(M[S['_imap']:S['_imap'] + 1024]); continue
    mem = bytearray(M[0:0x10000])
    ref = bytes(mem[S['_vbuf']:S['_vbuf'] + 2048])
    cyc, p, _ = sm83.run(mem, S[entry] if entry in S else int(entry, 16))
    if entry == '_render_asm' and bytes(mem[S['_vbuf']:S['_vbuf'] + 2048]) != ref:   # same state twice
        print('interpreter vbuf mismatch on view', views); sys.exit(1)
    prof.update(p); tot += cyc; views += 1
pb.stop(save=False)
by = collections.Counter()
for a, c in prof.items(): by[labels[a][0] if a in labels else clab(a)] += c
print('%s: %.0f T per view (%d views, vbuf checked)' % (entry, tot / views, views))
for lab, c in by.most_common(int(os.environ.get('TOPL', '30'))):
    print('  %-14s %8.0f T  %5.1f%%' % (lab, c / views, 100 * c / tot))
print('top instructions:')
for a, c in prof.most_common(int(os.environ.get('TOPI', '30'))):
    lab, t = labels.get(a, (clab(a), cln(a)))
    print('  %04X %-12s %-30s %7.0f T' % (a, lab, t, c / views))

if os.environ.get('BYLINE'):
    bl = collections.Counter()
    for a, c in prof.items():
        if a not in labels: bl[clab(a) + ' ' + cln(a)] += c
    for k, c in bl.most_common(int(os.environ.get('BYLINE'))):
        print('  %-40s %7.0f T' % (k, c / views))
