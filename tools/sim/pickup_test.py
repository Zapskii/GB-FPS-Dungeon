"""Pickup check for post_move(): build the game with -debug, start a run in
PyBoy, teleport onto a key and a health item.
usage: pickup_test.py [SRC_PARENT]  (default: repo root; builds in tools/sim/bw/pick)"""
import sys, os, subprocess, re, shutil
from pyboy import PyBoy

here = os.path.dirname(os.path.abspath(__file__))
repo = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', '..')
out = os.path.join(here, 'bw', 'pick')
shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
shutil.copytree(os.path.join(repo, 'src'), os.path.join(out, 'src'))
srcs = 'src/main.c src/isr.c src/render.s src/math.s src/rtabs.s src/billboard.s'.split()
r = subprocess.run(['docker', 'run', '--rm', '-u', '%d:%d' % (os.getuid(), os.getgid()), '-v', out + ':/w', '-w', '/w',
                    'gbdk-dev', '/opt/gbdk/bin/lcc', '-debug', '-Wm-yt0x03', '-Wm-ya1', '-Wl-j', '-o', 't.gb'] + srcs,
                   capture_output=True, text=True)
assert os.path.exists(os.path.join(out, 't.gb')), r.stdout + r.stderr
S = {}
for l in open(os.path.join(out, 't.noi')):
    p = l.split()
    if len(p) == 3 and p[0] == 'DEF': S[p[1]] = int(p[2], 16)
def sym(name):
    for k, v in S.items():
        if k == '_' + name or re.fullmatch(r'Fmain\$%s\$0_0\$0' % name, k): return v
    raise KeyError(name)

pb = PyBoy(os.path.join(out, 't.gb'), window='null', sound_emulated=False)
pb.set_emulation_speed(0)
M = pb.memory
def tick(n): pb.tick(n, False)
def w16(a, v): M[a] = v & 0xFF; M[a + 1] = v >> 8
tick(200)
pb.button_press('start'); tick(20); pb.button_release('start')
tick(900)                                   # attract exit + floor intro
n = M[sym('item_n')]
xs, ys = [M[sym('item_x') + i] for i in range(n)], [M[sym('item_y') + i] for i in range(n)]
kind = [M[sym('item_kind') + i] for i in range(n)]
taken = lambda i: M[sym('item_taken') + i]
HP, KEYS = sym('hp'), sym('keys')

def stand(i):
    w16(sym('cam_x'), (xs[i] << 8) + 128); w16(sym('cam_y'), (ys[i] << 8) + 128)

k = kind.index(0)                            # IT_KEY
keys0 = M[KEYS]
stand(k); tick(30)
assert taken(k) and M[KEYS] == keys0 + 1, ('key', taken(k), M[KEYS], keys0)
print('key: taken, keys %d -> %d' % (keys0, M[KEYS]))

if 1 in kind:                                # IT_HEALTH
    h = kind.index(1)
    M[HP] = 5; stand(h); tick(30)
    assert not taken(h), 'health taken at full hearts'
    M[HP] = 3; tick(30)
    assert taken(h) and M[HP] == 4, ('health', taken(h), M[HP])
    print('health: left at 5 hearts, taken once hurt (3 -> 4)')
else:
    print('no health item on this floor: health case skipped')
pb.stop(save=False)
print('ok')
