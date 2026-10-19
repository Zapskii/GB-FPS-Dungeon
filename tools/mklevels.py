#!/usr/bin/env python3
"""Procedural level generator for GB-FPS-Dungeon.

Builds 1-wide corridor mazes with a few small rooms and loops on a 32x32
grid (the raycaster reads close walls best), validates that the
key is reachable without opening the door and the stairs are reachable once
the door is opened, then emits src/levels.h.
"""
import sys
import random
from collections import deque

SIZE = 32
NLEVELS = 4

random.seed(0xD060)  # deterministic, reproducible builds


def in_bounds(r, c):
    return 0 <= r < SIZE and 0 <= c < SIZE


def carve_room(grid, r, c, h, w):
    for rr in range(r, r + h):
        for cc in range(c, c + w):
            if in_bounds(rr, cc):
                grid[rr][cc] = '.'


def carve_h_corridor(grid, r, c1, c2):
    for c in range(min(c1, c2), max(c1, c2) + 1):
        if in_bounds(r, c):
            grid[r][c] = '.'


def carve_v_corridor(grid, c, r1, r2):
    for r in range(min(r1, r2), max(r1, r2) + 1):
        if in_bounds(r, c):
            grid[r][c] = '.'


def connected(grid, start, goal, door_open, block=None):
    q = deque([start])
    seen = {start}
    while q:
        r, c = q.popleft()
        if (r, c) == goal:
            return True
        for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nr, nc = r + dr, c + dc
            if not in_bounds(nr, nc):
                continue
            if (nr, nc) in seen:
                continue
            if (nr, nc) == block:
                continue
            ch = grid[nr][nc]
            if ch in '#=':
                continue
            if ch == 'D' and not door_open:
                continue
            seen.add((nr, nc))
            q.append((nr, nc))
    return False


def find_feature(grid, ch):
    for r in range(SIZE):
        for c in range(SIZE):
            if grid[r][c] == ch:
                return (r, c)
    return None


def bfs_dist(grid, start):
    """Floor distance from start to every reachable cell (doors closed)."""
    dist = {start: 0}
    q = deque([start])
    while q:
        r, c = q.popleft()
        for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = (r + dr, c + dc)
            if in_bounds(*n) and n not in dist and grid[n[0]][n[1]] not in '#=D':
                dist[n] = dist[(r, c)] + 1
                q.append(n)
    return dist


def open_neighbours(grid, r, c):
    return [(r + dr, c + dc) for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1))
            if in_bounds(r + dr, c + dc) and grid[r + dr][c + dc] != '#']


# per level: (rooms, braid fraction) - later levels are twistier
LEVEL_STYLE = [(4, 0.5), (3, 0.4), (3, 0.3), (2, 0.2)]


def generate_map(level_no):
    """1-wide corridor maze (recursive backtracker on odd cells), a few small
    rooms, and some dead ends knocked through into loops. Corridors keep
    walls close, which is what reads best in the 128x64 raycaster."""
    grid = [['#' for _ in range(SIZE)] for _ in range(SIZE)]
    nrooms, braid = LEVEL_STYLE[level_no]
    cells = [(r, c) for r in range(1, SIZE - 1, 2) for c in range(1, SIZE - 1, 2)]

    # maze
    start = random.choice(cells)
    grid[start[0]][start[1]] = '.'
    stack = [start]
    while stack:
        r, c = stack[-1]
        nxt = [(r + dr, c + dc) for dr, dc in ((2, 0), (-2, 0), (0, 2), (0, -2))
               if 1 <= r + dr < SIZE - 1 and 1 <= c + dc < SIZE - 1
               and grid[r + dr][c + dc] == '#']
        if not nxt:
            stack.pop()
            continue
        nr, nc = random.choice(nxt)
        grid[(r + nr) // 2][(c + nc) // 2] = '.'
        grid[nr][nc] = '.'
        stack.append((nr, nc))

    # small rooms (3x3 or 3x5, on odd coordinates so they line up)
    for _ in range(nrooms):
        h, w = random.choice(((3, 3), (3, 5), (5, 3)))
        r = random.randrange(1, SIZE - 1 - h, 2)
        c = random.randrange(1, SIZE - 1 - w, 2)
        carve_room(grid, r, c, h, w)

    # braid: knock some dead ends through to a neighbouring corridor
    for r, c in cells:
        if len(open_neighbours(grid, r, c)) == 1 and random.random() < braid:
            walls = [(r + dr, c + dc, r + 2 * dr, c + 2 * dc)
                     for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1))
                     if 1 <= r + 2 * dr < SIZE - 1 and 1 <= c + 2 * dc < SIZE - 1
                     and grid[r + dr][c + dc] == '#' and grid[r + 2 * dr][c + 2 * dc] == '.']
            if walls:
                wr, wc, _, _ = random.choice(walls)
                grid[wr][wc] = '.'

    # P at a random cell; K at the farthest dead end; S at the next-farthest
    # dead end whose only neighbour is a plain corridor cell, which becomes D
    # (so the door seals exactly the stairs alcove).
    p = random.choice([x for x in cells if grid[x[0]][x[1]] == '.'])
    dist = bfs_dist(grid, p)
    ends = sorted((x for x in cells if x in dist and len(open_neighbours(grid, *x)) == 1),
                  key=lambda x: -dist[x])
    if len(ends) < 2:
        raise ValueError(f"level {level_no}: not enough dead ends")
    k = ends[0]
    s = d = None
    for e in ends[1:]:
        (nr, nc), = open_neighbours(grid, *e)
        if len(open_neighbours(grid, nr, nc)) == 2 and dist[e] >= dist[k] // 2:
            s, d = e, (nr, nc)
            break
    if s is None:
        raise ValueError(f"level {level_no}: no stairs alcove")
    grid[d[0]][d[1]] = 'D'
    grid[s[0]][s[1]] = 'S'
    grid[p[0]][p[1]] = 'P'
    grid[k[0]][k[1]] = 'K'

    # '=' variety on solid wall cells
    for r in range(2, SIZE - 2):
        for c in range(2, SIZE - 2):
            if grid[r][c] == '#' and random.random() < 0.15:
                grid[r][c] = '='

    # 6 enemies, at least 6 steps from the start so you get a head start
    far = [x for x, dd in dist.items() if dd >= 6 and grid[x[0]][x[1]] == '.']
    random.shuffle(far)
    for i, (r, c) in enumerate(far[:6]):
        grid[r][c] = str(i + 1)

    return grid, p, k, d, s


def validate(level_no, grid, p, k, d, s):
    if not connected(grid, p, k, door_open=False):
        raise ValueError(f"level {level_no}: key not reachable")
    if not connected(grid, p, s, door_open=True):
        raise ValueError(f"level {level_no}: stairs not reachable with door open")
    if connected(grid, p, s, door_open=False):
        raise ValueError(f"level {level_no}: stairs reachable without door")
    print(f"level {level_no}: OK P{p} K{k} D{d} S{s}", file=sys.stderr)


def grid_to_c_array(grid):
    rows = []
    for row in grid:
        rows.append(", ".join(str(ord(ch)) for ch in row))
    return "  {\n   " + ",\n   ".join(rows) + "\n  }"


def main():
    out = ["/* Generated levels (4 x 32x32 chars, flat row-major). */"]
    out.append("#ifndef LEVELS_H")
    out.append("#define LEVELS_H")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("#define NLEVELS 4u")
    out.append("")
    out.append("const uint8_t level_data[NLEVELS][1024] = {")
    parts = []
    for i in range(NLEVELS):
        while True:
            try:
                grid, p, k, d, s = generate_map(i)
                validate(i, grid, p, k, d, s)
                break
            except ValueError as e:
                print(f"retry: {e}", file=sys.stderr)
        parts.append(grid_to_c_array(grid))
    out.append(",\n".join(parts) + "\n};")
    out.append("#endif")
    print("\n".join(out))


if __name__ == "__main__":
    main()
