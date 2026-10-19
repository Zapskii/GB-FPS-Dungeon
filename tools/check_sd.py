# Self-check for render.s sd_lu/lf_pair: the log-domain side distance
# antilog[log8[f] + lrc[a] - 16] vs the exact f * rcp[a] >> 8.
#   python3 tools/check_sd.py
import re
s = open('src/tables.h').read()
def tab(name):
    body = re.search(name + r'\[\d+\] = \{(.*?)\};', s, re.S).group(1)
    return [int(x) for x in body.split(',') if x.strip()]
rcp, log8, antilog, lrc = tab('rcp'), tab('log8'), tab('antilog'), tab('lrc')
worst = 0
for lo in range(256):                      # camera fraction, both ray signs
    for f, lf in ((256 - lo, 248 if lo == 0 else log8[256 - lo]), (lo, log8[lo])):
        for a in range(128):
            got = antilog[max(lf + lrc[a], 16) - 16]
            want = f * rcp[a] >> 8
            if want >= 64:                 # >= 1/4 cell: relative error matters
                worst = max(worst, abs(got - want) / want)
            else:                          # tiny: absolute error (f=0 -> f=1)
                assert abs(got - want) <= max(rcp[a] >> 8, 4) + 2, (f, a, got, want)
assert worst < 0.04, worst
print('sd_lu ok, worst relative error %.2f%%' % (100 * worst))
