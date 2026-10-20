/* Enemy pathfinding: BFS distance field from the player's cell over floor
 * cells, enemies walk downhill along corridors. Include after imap, cam_x/y,
 * ex/ey/ealive, NENEMY and blocked() are declared. */
#ifndef AI_H
#define AI_H

#include <string.h>

#define AI_MAX_DIST 10u     /* BFS depth cap: farther enemies idle */
#define AI_QLEN     128u    /* max cells visited per rebuild (bounds cost) */
#define AI_SPEED    6u     /* grunt step per AI tick; caller sets ai_sp per enemy */
#define AI_FAR      255u

static uint8_t ai_dist[1024];
static uint16_t ai_q[AI_QLEN];
static uint16_t ai_pcell;   /* player cell of the current field */
static uint8_t ai_n;        /* cells the last BFS wrote: ai_q[0..ai_n) */
static uint8_t ai_d;        /* BFS: distance of the cells being queued */
static uint8_t ai_sp = AI_SPEED;   /* step size for the next ai_step() */

static void ai_reset(void)
{
    memset(ai_dist, AI_FAR, sizeof(ai_dist));
    ai_n = 0u;
    ai_pcell = 0xFFFFu;
}

/* BFS from cell s (in de, sdcccall 1): AI_MAX_DIST (10) steps, at most
 * AI_QLEN (128) cells, the rest stay AI_FAR. Same results as the old C loop:
 * stops early once the queue is full or the popped cell is at the depth
 * cap (FIFO, so every later cell is too). Instead of a 1 KB memset it first
 * un-writes the previous run: ai_q is linear, never wraps, so ai_q[0..ai_n)
 * lists every cell that run set. Literals 10/0xff/256 mirror AI_MAX_DIST,
 * AI_FAR and 2*AI_QLEN (the preprocessor would mangle 10u in asm). */
static void ai_bfs(uint16_t s) __naked
{
    (void)s;
    __asm
    push de
    ld   a, (#_ai_n)                ; un-write the previous field
    or   a, a
    jr   z, 2$
    ld   c, a
    ld   hl, #_ai_q
1$:
    ld   a, (hl+)
    add  a, #<(_ai_dist)
    ld   e, a
    ld   a, (hl+)
    adc  a, #>(_ai_dist)
    ld   d, a
    ld   a, #0xff
    ld   (de), a
    dec  c
    jr   nz, 1$
2$:
    pop  bc                         ; bc = s
    ld   hl, #_ai_dist
    add  hl, bc
    ld   (hl), #0
    ld   hl, #_ai_q
    ld   a, c
    ld   (hl+), a
    ld   a, b
    ld   (hl+), a
    ld   e, l                       ; de = queue tail
    ld   d, h
    ld   hl, #_ai_q                 ; hl = queue head
3$:
    ld   a, l
    cp   a, e
    jr   nz, 4$
    ld   a, h
    cp   a, d
    jp   z, 9$
4$:
    ld   a, (hl+)                   ; bc = cell c
    ld   c, a
    ld   a, (hl+)
    ld   b, a
    push hl
    ld   hl, #_ai_dist
    add  hl, bc
    ld   a, (hl)
    cp   a, #10
    jp   nc, 8$
    inc  a
    ld   (#_ai_d), a
    ld   hl, #_imap+1
    add  hl, bc
    ld   a, (hl)
    or   a, a
    jr   nz, 5$
    ld   hl, #_ai_dist+1
    add  hl, bc
    ld   a, (hl)
    inc  a
    jr   nz, 5$
    ld   a, (#_ai_d)
    ld   (hl), a
    ld   hl, #0x0001
    add  hl, bc
    ld   a, l
    ld   (de), a
    inc  de
    ld   a, h
    ld   (de), a
    inc  de
    ld   a, e
    cp   a, #<(_ai_q+256)
    jr   nz, 5$
    ld   a, d
    cp   a, #>(_ai_q+256)
    jp   z, 8$
5$:
    ld   hl, #_imap-1
    add  hl, bc
    ld   a, (hl)
    or   a, a
    jr   nz, 6$
    ld   hl, #_ai_dist-1
    add  hl, bc
    ld   a, (hl)
    inc  a
    jr   nz, 6$
    ld   a, (#_ai_d)
    ld   (hl), a
    ld   hl, #0xffff
    add  hl, bc
    ld   a, l
    ld   (de), a
    inc  de
    ld   a, h
    ld   (de), a
    inc  de
    ld   a, e
    cp   a, #<(_ai_q+256)
    jr   nz, 6$
    ld   a, d
    cp   a, #>(_ai_q+256)
    jp   z, 8$
6$:
    ld   hl, #_imap+32
    add  hl, bc
    ld   a, (hl)
    or   a, a
    jr   nz, 7$
    ld   hl, #_ai_dist+32
    add  hl, bc
    ld   a, (hl)
    inc  a
    jr   nz, 7$
    ld   a, (#_ai_d)
    ld   (hl), a
    ld   hl, #0x0020
    add  hl, bc
    ld   a, l
    ld   (de), a
    inc  de
    ld   a, h
    ld   (de), a
    inc  de
    ld   a, e
    cp   a, #<(_ai_q+256)
    jr   nz, 7$
    ld   a, d
    cp   a, #>(_ai_q+256)
    jp   z, 8$
7$:
    ld   hl, #_imap-32
    add  hl, bc
    ld   a, (hl)
    or   a, a
    jr   nz, 10$
    ld   hl, #_ai_dist-32
    add  hl, bc
    ld   a, (hl)
    inc  a
    jr   nz, 10$
    ld   a, (#_ai_d)
    ld   (hl), a
    ld   hl, #0xffe0
    add  hl, bc
    ld   a, l
    ld   (de), a
    inc  de
    ld   a, h
    ld   (de), a
    inc  de
    ld   a, e
    cp   a, #<(_ai_q+256)
    jr   nz, 10$
    ld   a, d
    cp   a, #>(_ai_q+256)
    jp   z, 8$
10$:
    pop  hl
    jp   3$
8$:
    pop  hl
9$:
    ld   a, e                       ; ai_n = (tail - ai_q) / 2
    sub  a, #<(_ai_q)
    ld   e, a
    ld   a, d
    sbc  a, #>(_ai_q)
    srl  a
    rr   e
    ld   a, e
    ld   (#_ai_n), a
    ret
    __endasm;
}

static void ai_build(uint16_t s)
{
    PROF_BFS;
    ai_bfs(s);
}

static void ai_update(void)
{
    uint16_t c = ((cam_y >> 8) << 5) + (cam_x >> 8);
    if (c != ai_pcell) {
        ai_pcell = c;
        ai_build(c);
    }
}

/* Signed step from p toward t, at most ai_sp, never overshooting. */
static int16_t ai_toward(uint16_t p, uint16_t t)
{
    int16_t d = (int16_t)(t - p);
    if (d > (int16_t)ai_sp) return (int16_t)ai_sp;
    if (d < -(int16_t)ai_sp) return -(int16_t)ai_sp;
    return d;
}

static void ai_step(uint8_t i)
{
    uint16_t x, y, c, n, best, nx, ny;
    uint8_t bd, horiz;

    x = ex[i];
    y = ey[i];
    c = ((y >> 8) << 5) + (x >> 8);
    bd = ai_dist[c];
    if (bd == AI_FAR) return;

    if (bd == 0u) {                         /* same cell: go straight at player */
        nx = x + (uint16_t)ai_toward(x, cam_x);
        ny = y + (uint16_t)ai_toward(y, cam_y);
        if (!blocked(nx, y)) x = nx;
        if (!blocked(x, ny)) y = ny;
        ex[i] = x;
        ey[i] = y;
        return;
    }

    /* downhill neighbour (walls/unreached are 255, never chosen) */
    best = c;
    if (ai_dist[c + 1u] < bd) { bd = ai_dist[c + 1u]; best = c + 1u; }
    if (ai_dist[c - 1u] < bd) { bd = ai_dist[c - 1u]; best = c - 1u; }
    if (ai_dist[c + 32u] < bd) { bd = ai_dist[c + 32u]; best = c + 32u; }
    if (ai_dist[c - 32u] < bd) { bd = ai_dist[c - 32u]; best = c - 32u; }
    if (best == c) return;

    nx = x;
    ny = y;
    horiz = 0u;
    if (best == c + 1u) horiz = 1u;
    if (best == c - 1u) horiz = 1u;
    if (horiz) {
        n = (y & 0xFF00u) + 128u;           /* centre y first */
        if (y != n) {
            ny = y + (uint16_t)ai_toward(y, n);
        } else {
            n = ((best & 31u) << 8) + 128u;
            nx = x + (uint16_t)ai_toward(x, n);
        }
    } else {
        n = (x & 0xFF00u) + 128u;           /* centre x first */
        if (x != n) {
            nx = x + (uint16_t)ai_toward(x, n);
        } else {
            n = ((best >> 5) << 8) + 128u;
            ny = y + (uint16_t)ai_toward(y, n);
        }
    }
    if (!blocked(nx, ny)) {
        ex[i] = nx;
        ey[i] = ny;
    }
}

#endif
