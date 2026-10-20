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

/* Enemy step (sdcccall 1: i in a): walk downhill on the BFS field, centre in
 * the cell, move at most ai_sp px. Same decisions as the old C version,
 * including the same-cell case and its two blocked() checks (inlined as
 * imap[cell] != 0). Writes only ex[i]/ey[i]. */
static void ai_step(uint8_t i) __naked
{
    (void)i;
    __asm
    push af                     ; keep i for the store
    ld   b, a
    add  a, a                   ; 2*i
    ld   e, a
    ld   d, #0
    ld   hl, #_ey
    add  hl, de
    ld   a, (hl+)
    ld   b, (hl)
    ld   c, a                   ; bc = y
    ld   hl, #_ex
    add  hl, de
    ld   a, (hl+)               ; x low
    ld   h, (hl)                ; x high
    ld   l, a                   ; hl = x
    ld   a, b                   ; cell = (y >> 8) << 5 | (x >> 8)
    srl  a
    srl  a
    srl  a
    ld   d, a
    ld   a, b
    add  a, a
    add  a, a
    add  a, a
    add  a, a
    add  a, a
    add  a, h
    ld   e, a                   ; de = cell
    push hl                     ; x
    push bc                     ; y
    ld   hl, #_ai_dist
    add  hl, de
    ld   a, (hl)                ; bd
    or   a, a
    jp   z, 2$
    cp   a, #0xff
    jp   z, 9$                  ; AI_FAR: idle
    ld   b, a                   ; bd
    ld   c, #0                  ; best: 0 none, 1 +1, 2 -1, 3 +32, 4 -32
    inc  hl
    ld   a, (hl)
    cp   a, b
    jr   nc, 3$
    ld   b, a
    ld   c, #1
3$: dec  hl
    dec  hl
    ld   a, (hl)
    cp   a, b
    jr   nc, 4$
    ld   b, a
    ld   c, #2
4$: inc  hl
    ld   a, l
    add  a, #32
    ld   l, a
    ld   a, h
    adc  a, #0
    ld   h, a
    ld   a, (hl)
    cp   a, b
    jr   nc, 5$
    ld   b, a
    ld   c, #3
5$: ld   a, l
    sub  a, #64
    ld   l, a
    ld   a, h
    sbc  a, #0
    ld   h, a
    ld   a, (hl)
    cp   a, b
    jr   nc, 6$
    ld   b, a
    ld   c, #4
6$: ld   a, c
    or   a, a
    jp   z, 9$                  ; best == c
    cp   a, #3
    jr   c, 7$                  ; +1 / -1: horizontal
    jr   nz, 8$                 ; -32
    ld   a, e                   ; +32
    add  a, #32
    ld   e, a
    ld   a, d
    adc  a, #0
    ld   d, a
    jr   10$
8$: ld   a, e
    sub  a, #32
    ld   e, a
    ld   a, d
    sbc  a, #0
    ld   d, a
    jr   10$
7$: ld   a, c
    dec  a
    jr   nz, 11$
    inc  de
    jr   10$
11$:dec  de
10$:
    ld   a, c
    cp   a, #3
    jr   c, 12$                 ; horizontal
    pop  bc                     ; vertical: centre x first
    pop  hl
    ld   a, l
    cp   a, #128
    jr   z, 13$
    ld   a, #128                ; nx = x + clamp(128 - xl)
    sub  a, l
    ld   e, a
    ld   a, #0
    sbc  a, #0
    ld   d, a
    call 20$
    ld   a, l
    add  a, e
    ld   l, a
    ld   a, h
    adc  a, d
    ld   h, a
    jr   14$
13$:                            ; x centred: move y toward the best row
    ld   a, d                   ; target high = best >> 5
    add  a, a
    add  a, a
    add  a, a
    ld   d, a
    ld   a, e
    srl  a
    srl  a
    srl  a
    srl  a
    srl  a
    or   a, d
    ld   d, a
    ld   a, #128                ; ny = y + clamp(target - y)
    sub  a, c
    ld   e, a
    ld   a, d
    sbc  a, b
    ld   d, a
    call 20$
    ld   a, c
    add  a, e
    ld   c, a
    ld   a, b
    adc  a, d
    ld   b, a
    jr   14$
12$:                            ; horizontal: centre y first
    pop  bc
    pop  hl
    ld   a, c
    cp   a, #128
    jr   z, 15$
    ld   a, #128                ; ny = y + clamp(128 - yl)
    sub  a, c
    ld   e, a
    ld   a, #0
    sbc  a, #0
    ld   d, a
    call 20$
    ld   a, c
    add  a, e
    ld   c, a
    ld   a, b
    adc  a, d
    ld   b, a
    jr   14$
15$:                            ; y centred: move x toward the best column
    ld   a, e                   ; target high = best & 31
    and  a, #31
    ld   d, a
    ld   a, #128                ; nx = x + clamp(target - x)
    sub  a, l
    ld   e, a
    ld   a, d
    sbc  a, h
    ld   d, a
    call 20$
    ld   a, l
    add  a, e
    ld   l, a
    ld   a, h
    adc  a, d
    ld   h, a
14$:
    push hl                     ; blocked(nx, ny)?
    ld   a, b
    srl  a
    srl  a
    srl  a
    ld   d, a
    ld   a, b
    add  a, a
    add  a, a
    add  a, a
    add  a, a
    add  a, a
    add  a, h
    ld   e, a
    ld   hl, #_imap
    add  hl, de
    ld   a, (hl)
    pop  hl
    or   a, a
    jp   nz, 17$
    jp   16$
2$:                             ; same cell: walk straight at the player
    pop  bc
    pop  hl
    push hl                     ; old x
    ld   a, (#_cam_x)
    ld   e, a
    ld   a, (#_cam_x+1)
    ld   d, a
    ld   a, e
    sub  a, l
    ld   e, a
    ld   a, d
    sbc  a, h
    ld   d, a
    call 20$
    ld   a, l
    add  a, e
    ld   l, a
    ld   a, h
    adc  a, d
    ld   h, a                   ; hl = nx
    push hl
    ld   a, b                   ; blocked(nx, y)?
    srl  a
    srl  a
    srl  a
    ld   d, a
    ld   a, b
    add  a, a
    add  a, a
    add  a, a
    add  a, a
    add  a, a
    add  a, h
    ld   e, a
    ld   hl, #_imap
    add  hl, de
    ld   a, (hl)
    pop  hl
    or   a, a
    jr   z, 21$
    pop  hl                     ; blocked: keep x
    jr   22$
21$:pop  de                     ; free: drop the saved x
22$:
    push bc                     ; old y
    ld   a, (#_cam_y)
    ld   e, a
    ld   a, (#_cam_y+1)
    ld   d, a
    ld   a, e
    sub  a, c
    ld   e, a
    ld   a, d
    sbc  a, b
    ld   d, a
    call 20$
    ld   a, c
    add  a, e
    ld   c, a
    ld   a, b
    adc  a, d
    ld   b, a                   ; bc = ny
    push hl                     ; x
    push bc                     ; ny
    ld   a, b                   ; blocked(x, ny)?
    srl  a
    srl  a
    srl  a
    ld   d, a
    ld   a, b
    add  a, a
    add  a, a
    add  a, a
    add  a, a
    add  a, a
    add  a, h
    ld   e, a
    ld   hl, #_imap
    add  hl, de
    ld   a, (hl)
    pop  bc
    pop  hl
    or   a, a
    jr   z, 23$
    pop  bc                     ; blocked: keep y
    jr   16$
23$:pop  de                     ; free: drop the saved y
16$:                            ; ex[i] = x (hl), ey[i] = y (bc)
    pop  af
    add  a, a
    ld   e, a
    ld   d, #0
    push hl
    ld   hl, #_ey
    add  hl, de
    ld   a, c
    ld   (hl+), a
    ld   a, b
    ld   (hl), a
    pop  hl
    ld   b, h
    ld   c, l
    ld   hl, #_ex
    add  hl, de
    ld   a, c
    ld   (hl+), a
    ld   a, b
    ld   (hl), a
    ret
17$:                            ; blocked: write nothing
    pop  af
    ret
9$:                             ; idle: drop x, y, i
    pop  bc
    pop  hl
    pop  af
    ret
20$:                            ; de = clamp(de, +-ai_sp)
    push hl
    push bc
    ld   a, (#_ai_sp)
    ld   b, a
    ld   h, d
    ld   a, e
    sub  a, b
    ld   c, a
    ld   a, h
    sbc  a, #0
    or   a, a
    jr   z, 24$
    bit  7, a
    jr   nz, 25$
    jr   26$                    ; de > ai_sp
24$: ld   a, c
    or   a, a
    jr   nz, 26$
    jr   25$
25$:                            ; de < -ai_sp?
    ld   a, e
    add  a, b
    ld   c, a
    ld   a, h
    adc  a, #0
    bit  7, a
    jr   z, 27$
    ld   a, #0
    sub  a, b
    ld   e, a
    ld   d, #0xff
    jr   27$
26$: ld   e, b
    ld   d, #0
27$: pop  bc
    pop  hl
    ret
    __endasm;
}

#endif
