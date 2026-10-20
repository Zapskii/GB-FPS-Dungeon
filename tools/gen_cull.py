"""cull_r[top] for src/tables.h: project()'s box radius in cells when the
image's highest wall top row (r_top max, 0..31) is `top`.

A visible billboard sits nearer than the wall at its column (fwd>>5 <= zb)
and inside the 90 deg FOV (|lat| < fwd), so its distance is < sqrt(2) *
(zb_max + 1) / 8 cells. Every ray's top = (64 - ht[perp>>3]) >> 1 and ht
never grows with distance, so top <= T bounds perp>>3 by the last n with
(64 - ht[n]) >> 1 <= T, and zb = perp>>5 by that n >> 2. 1.45 (not 1.42)
and +64 (1/4 cell) cover project()'s fixed-point error. 10 = no cull."""
import re, os
src = open(os.path.join(os.path.dirname(__file__), '..', 'src', 'tables.h')).read()
ht = [int(v) for v in re.search(r'ht\[1024\] = \{(.*?)\};', src, re.S).group(1).replace('u', '').split(',') if v.strip()]
assert len(ht) == 1024 and all(a >= b for a, b in zip(ht, ht[1:]))
out = []
for t in range(32):
    n = max(i for i in range(1024) if (64 - ht[i]) >> 1 <= t) if (64 - ht[0]) >> 1 <= t else 0
    if n == 1023:
        out.append(10)                 # up to the clamp (zb 255): no bound
        continue
    bound = 1.45 * ((n >> 2) + 1) * 32 + 64          # 8.8 units
    out.append(min(10, int(bound) // 256 + 1))
print('const uint8_t cull_r[32] = {%s};' % ', '.join('%du' % r for r in out))
