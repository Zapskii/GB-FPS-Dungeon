# Generates frame tile bytes (printed as C) for frame.h.
T = L = [3,1,2,2,2,2,2,3]   # top/left side, outer->inner: outline, highlight, body, inner lip
B = R = [3,3,2,2,2,2,1,3]   # bottom/right: thicker shadow outside, lit inner lip
RIVET = {(3,3):1,(4,3):2,(3,4):2,(4,4):3}

def brick(x, y):
    if y in (0, 4): return 2
    if y < 4 and x == 0: return 2
    if y > 4 and x == 4: return 2
    return 3

def edge(sides, rivet=False):
    def f(x, y):
        if rivet and (x, y) in RIVET: return RIVET[(x, y)]
        best = None
        for d, prof in [(y if 't' in sides else 99, T), (7-y if 'b' in sides else 99, B),
                        (x if 'l' in sides else 99, L), (7-x if 'r' in sides else 99, R)]:
            if d < 99 and (best is None or d < best[0]): best = (d, prof)
        return best[1][best[0]]
    return f

tiles = [brick, edge('t'), edge('t', True), edge('b'), edge('b', True), edge('l'), edge('r'),
         edge('tl', True), edge('tr', True), edge('bl', True), edge('br', True)]
names = ['brick','top','top+rivet','bottom','bottom+rivet','left','right','TL','TR','BL','BR']
for f, n in zip(tiles, names):
    b = []
    for y in range(8):
        lo = hi = 0
        for x in range(8):
            c = f(x, y)
            lo |= (c & 1) << (7-x); hi |= (c >> 1) << (7-x)
        b += [lo, hi]
    print('    ' + ','.join('0x%02Xu' % v for v in b) + ', /* %s */' % n)
