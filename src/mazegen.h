/* On-cartridge level generator (C port of the old tools/mklevels.py), so
 * every run gets new mazes. Writes a 32x32 char map in the load_level()
 * grammar: '#' '=' wall, 'D' door, 'K' key, 'S' stairs, 'P' start,
 * '1'..'5' grunts, 'B' bats, 'X' brutes, 'H' health, 'G' gold, 'M' map,
 * '.' floor.
 *
 * 1-wide corridor maze (recursive backtracker on odd cells), a few small
 * rooms, some dead ends knocked through into loops. Key in the farthest
 * dead end; stairs in another far dead end whose only neighbour is a plain
 * corridor cell, which becomes the door - so the door seals exactly the
 * stairs alcove and the key is always reachable without it (no flood-fill
 * validation needed).
 *
 * Scratch is passed in: dist (1024 bytes) and q (512 x uint16, used as the
 * backtracker stack and then as the BFS ring). Seed with initrand() first.
 */
#ifndef MAZEGEN_H
#define MAZEGEN_H

#include <stdint.h>
#include <string.h>
#include <rand.h>

#define MZ_QMASK 511u

/* per floor: rooms, braid chance (of 256) - later floors are twistier */
static const uint8_t mz_rooms[4] = {4u, 3u, 3u, 2u};
static const uint8_t mz_braid[4] = {128u, 102u, 77u, 51u};

static uint8_t mz_rnd(uint8_t n)
{
    return (uint8_t)(rand() % n);
}

static uint8_t mz_open(const uint8_t *m, uint16_t i)
{
    uint8_t n = 0u;
    if (m[i + 1u] != '#') n++;
    if (m[i - 1u] != '#') n++;
    if (m[i + 32u] != '#') n++;
    if (m[i - 32u] != '#') n++;
    return n;
}

/* Odd-cell neighbour of i in direction d (0 E, 1 W, 2 S, 3 N) two steps
 * away, or 0 if outside the 1..29 cell area. */
static uint16_t mz_step2(uint16_t i, uint8_t d)
{
    uint8_t c = (uint8_t)(i & 31u), r = (uint8_t)(i >> 5);

    if (d == 0u) { if (c > 27u) return 0u; return i + 2u; }
    if (d == 1u) { if (c < 3u) return 0u; return i - 2u; }
    if (d == 2u) { if (r > 27u) return 0u; return i + 64u; }
    if (r < 3u) return 0u;
    return i - 64u;
}

static uint16_t mz_cell(void)            /* random odd cell */
{
    uint8_t r = (uint8_t)(1u + 2u * mz_rnd(15u)), c = (uint8_t)(1u + 2u * mz_rnd(15u));
    return ((uint16_t)r << 5) + c;
}

/* Put ch on a random '.' cell at least min steps from the start. */
static void mz_place(uint8_t *m, const uint8_t *dist, uint8_t ch, uint8_t min)
{
    uint16_t i;
    uint8_t tries;

    for (tries = 0u; tries < 200u; tries++) {
        i = (uint16_t)(randw() & 1023u);
        if (m[i] != '.') continue;
        if (dist[i] == 255u) continue;
        if (dist[i] < min) continue;
        m[i] = ch;
        return;
    }
}

/* Returns 0 if this attempt found no stairs alcove (caller retries). */
static uint8_t mz_try(uint8_t floor, uint8_t *m, uint8_t *dist, uint16_t *q)
{
    uint16_t i, n, p, k, s, d, sp, head, tail, cand[4];
    uint8_t dir, nc, r, c, h, w, rr, cc, best;

    memset(m, '#', 1024u);

    /* maze */
    i = mz_cell();
    m[i] = '.';
    q[0] = i;
    sp = 1u;
    while (sp) {
        i = q[sp - 1u];
        nc = 0u;
        for (dir = 0u; dir < 4u; dir++) {
            n = mz_step2(i, dir);
            if (n) {
                if (m[n] == '#') { cand[nc] = n; nc++; }
            }
        }
        if (!nc) { sp--; continue; }
        n = cand[mz_rnd(nc)];
        m[(i + n) >> 1] = '.';
        m[n] = '.';
        q[sp] = n;
        sp++;
    }

    /* small rooms: 3x3, 3x5 or 5x3 on odd coordinates */
    for (r = 0u; r < mz_rooms[floor]; r++) {
        dir = mz_rnd(3u);
        h = 3u; w = 3u;
        if (dir == 1u) w = 5u;
        if (dir == 2u) h = 5u;
        rr = (uint8_t)(1u + 2u * mz_rnd((uint8_t)((31u - h) >> 1)));
        cc = (uint8_t)(1u + 2u * mz_rnd((uint8_t)((31u - w) >> 1)));
        for (i = 0u; i < h; i++)
            for (n = 0u; n < w; n++)
                m[((uint16_t)(rr + i) << 5) + cc + n] = '.';
    }

    /* braid: knock some dead ends through to a neighbouring corridor */
    for (r = 1u; r < 30u; r += 2u) {
        for (c = 1u; c < 30u; c += 2u) {
            i = ((uint16_t)r << 5) + c;
            if (mz_open(m, i) != 1u) continue;
            if (rand() >= mz_braid[floor]) continue;
            nc = 0u;
            for (dir = 0u; dir < 4u; dir++) {
                n = mz_step2(i, dir);
                if (n) {
                    if (m[(i + n) >> 1] == '#') {
                        if (m[n] != '#') { cand[nc] = (i + n) >> 1; nc++; }
                    }
                }
            }
            if (nc) m[cand[mz_rnd(nc)]] = '.';
        }
    }

    /* BFS distances from the start (saturating at 254; 255 = unreached) */
    p = mz_cell();
    memset(dist, 255u, 1024u);
    dist[p] = 0u;
    q[0] = p;
    head = 0u;
    tail = 1u;
    while (head != tail) {
        i = q[head];
        head = (head + 1u) & MZ_QMASK;
        best = dist[i];
        if (best < 254u) best++;
        for (dir = 0u; dir < 4u; dir++) {
            if (dir == 0u) n = i + 1u;
            else if (dir == 1u) n = i - 1u;
            else if (dir == 2u) n = i + 32u;
            else n = i - 32u;
            if (m[n] == '#') continue;
            if (dist[n] != 255u) continue;
            dist[n] = best;
            q[tail] = n;
            tail = (tail + 1u) & MZ_QMASK;
        }
    }

    /* key: farthest dead end */
    k = 0u;
    best = 0u;
    for (r = 1u; r < 30u; r += 2u) {
        for (c = 1u; c < 30u; c += 2u) {
            i = ((uint16_t)r << 5) + c;
            if (i == p) continue;
            if (dist[i] == 255u) continue;
            if (mz_open(m, i) != 1u) continue;
            if (dist[i] >= best) { best = dist[i]; k = i; }
        }
    }
    if (!k) return 0u;

    /* stairs: farthest other dead end behind a 2-way corridor cell */
    s = 0u;
    d = 0u;
    w = 0u;
    for (r = 1u; r < 30u; r += 2u) {
        for (c = 1u; c < 30u; c += 2u) {
            i = ((uint16_t)r << 5) + c;
            if (i == p) continue;
            if (i == k) continue;
            if (dist[i] == 255u) continue;
            if (dist[i] < (dist[k] >> 1)) continue;
            if (dist[i] < w) continue;
            if (mz_open(m, i) != 1u) continue;
            if (m[i + 1u] != '#') n = i + 1u;
            else if (m[i - 1u] != '#') n = i - 1u;
            else if (m[i + 32u] != '#') n = i + 32u;
            else n = i - 32u;
            if (n == p) continue;
            if (mz_open(m, n) != 2u) continue;
            w = dist[i];
            s = i;
            d = n;
        }
    }
    if (!s) return 0u;

    m[d] = 'D';
    m[s] = 'S';
    m[p] = 'P';
    m[k] = 'K';

    /* '=' variety on interior walls */
    for (i = 66u; i < 958u; i++)
        if (m[i] == '#')
            if (rand() < 38u) m[i] = '=';

    /* enemies at least 6 steps away, 8 at most (NENEMY): floor + 1 bats,
     * brutes from floor 3 (1, then 2), grunts fill up to 6 (5 + 3 on the
     * last floor); items at least 4, the map 8 */
    h = 0u;
    if (floor >= 2u) h = (uint8_t)(floor - 1u);
    w = (uint8_t)(5u - h);
    if (floor == 3u) w = 2u;
    for (c = 0u; c < w; c++) mz_place(m, dist, (uint8_t)('1' + c), 6u);
    for (c = 0u; c < h; c++) mz_place(m, dist, 'X', 6u);
    for (c = 0u; c <= floor; c++) mz_place(m, dist, 'B', 6u);
    mz_place(m, dist, 'H', 4u);
    mz_place(m, dist, 'H', 4u);
    mz_place(m, dist, 'G', 4u);
    mz_place(m, dist, 'G', 4u);
    mz_place(m, dist, 'M', 8u);
    return 1u;
}

static void maze_generate(uint8_t floor, uint8_t *m, uint8_t *dist, uint16_t *q)
{
    while (!mz_try(floor, m, dist, q)) ;
}

#endif
