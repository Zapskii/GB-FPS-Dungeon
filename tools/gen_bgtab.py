# ceiling/floor pattern per row y: colour of pixel x (0..7)
def px(y, x):
    if y < 10: return 2                      # ceiling, near: solid dark
    if y < 20: return 2 if y % 2 == 0 else 3 # horizontal lines
    if y < 26: return 2 if y % 4 == 0 else 3 # sparse lines
    if y < 41: return 3                      # horizon band (32 is always wall)
    if y < 47: return 2 if (y % 2 == 0 and (x + y // 2) % 2 == 0) else 3  # 25% dots
    if y < 53: return 2 if (x + y) % 2 == 0 else 3   # 2/3 checker
    if y < 59: return 2                      # solid dark
    return 1 if (x + y) % 2 == 0 else 2      # 1/2 checker, nearest
out = []
for y in range(64):
    lo = hi = 0
    for x in range(8):
        c = px(y, x)
        lo |= (c & 1) << (7 - x); hi |= (c >> 1) << (7 - x)
    out.append("0x%02X,0x%02X" % (lo, hi))
for i in range(0, 64, 4):
    print("        .db     " + ", ".join(out[i:i+4]) + "   ; rows %d-%d" % (i, i + 3))
