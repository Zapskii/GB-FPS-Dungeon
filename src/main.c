/* GB-FPS-Dungeon — pixel raycaster, C game shell.
 *
 * render.s casts 64 rays and draws a 128x64 bitmap into WRAM (vbuf);
 * copy_vbuf() moves it into the hidden VRAM tile bank and the main loop
 * flips banks with LCDC.4 in VBlank. C owns init, input, movement,
 * enemies, sprites and the HUD.
 *
 * VRAM layout: the viewport is 16x8 BG tiles (indices 0..127, column-
 * major). With LCDC.4 = 1 they read 0x8000-0x87FF (bank A), with 0 they
 * read 0x9000-0x97FF (bank B). Indices 128+ (0x8800-0x8FFF) are the same
 * in both modes: border, HUD and sprite tiles live there.
 */
#include <gbdk/platform.h>
#include <gb/gb.h>
#include <stdint.h>
#include "tables.h"
#include "hud.h"
#include "sprites.h"
#include <rand.h>
#include "mazegen.h"

#define NLEVELS 4u                      /* floors per run, generated on load */
#include "sfx.h"
#include "frame.h"
#include "gun.h"

#define VIEW_X0  16u      /* viewport top-left on screen, px */
#define VIEW_Y0  32u
#define HORIZON  64       /* screen y of the horizon */

#define T_BLACK  128u
#define T_WHITE  129u

/* HUD tiles (base 160): digits, letters, heart/key, inverted NESW */
#define HUD_BASE 160u
#define HUD_DIGIT(n) (HUD_BASE + (n))
#define HUD_N (HUD_BASE + 10u)
#define HUD_E (HUD_BASE + 11u)
#define HUD_S (HUD_BASE + 12u)
#define HUD_W (HUD_BASE + 13u)
#define HUD_K (HUD_BASE + 14u)
#define HUD_F (HUD_BASE + 15u)
#define HUD_HEART_FULL  (HUD_BASE + 16u)
#define HUD_HEART_EMPTY (HUD_BASE + 17u)
#define HUD_KEY         (HUD_BASE + 18u)
#define HUD_INV_N (HUD_BASE + 19u)
#define HUD_INV_E (HUD_BASE + 20u)
#define HUD_INV_S (HUD_BASE + 21u)
#define HUD_INV_W (HUD_BASE + 22u)
#define HUD_GOLD  183u                  /* gem, after the HUD set */
#define BUILD     32u                   /* HUD row 1 middle, 2 digits: bump every hand-off */
#define HUD_MAX_HP 5u

const uint8_t tiles[2][16] = {
    {0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF,
     0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF},   /* T_BLACK */
    {0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00,
     0x00,0x00, 0x00,0x00, 0x00,0x00, 0x00,0x00},   /* T_WHITE */
};

extern volatile uint8_t vbl_count, flash_n, flash_pal, flip_req, images;
void vbl_handler(void);
void render_asm(void);
void copy_vbuf(void);
void copy_view(void);
uint8_t check_view(void);              /* render.s, make CHECK=1 */
extern volatile uint8_t chk_err;

/* Scratch read/written by render.s — keep names in sync with the asm. */
uint8_t r_k;
int16_t r_rdx, r_rdy, r_stx, r_sty;   /* column-0 ray dir + per-column step, 8.8 */
uint8_t r_tc, r_cb[2], r_pt[2], r_mxt, r_mnb, r_lo, r_hi;
uint16_t r_dst;
uint8_t r_vh[64];            /* copy_view: span last copied per bank+column */
extern uint8_t c_key[512];   /* copy_view: column keys per bank (render.s) */
uint8_t vbuf[2048];          /* 128x64 bitmap in column-major tile order */

/* Defined in render.s, which relies on their order (per-ray arrays at
 * 64-byte strides, seen[] right after imap[]). */
extern uint8_t r_top[64], r_bot[64], r_zb[64];
extern uint8_t imap[1024];   /* char map -> collision/ray map */
extern uint8_t seen[1024];   /* automap: cells walked next to or seen by a ray */
static uint16_t seen_at = 0xFFFFu;
uint16_t cam_x, cam_y;       /* 8.8, 256 = one cell */
uint8_t  cam_a;              /* 0..255, 256 = full turn, 0 = +x */

/* HUD/game state (early: move()/load_level/hud all use it). */
static uint8_t gun_bob = 0u;
static uint8_t hp = HUD_MAX_HP, keys = 0u, floor_no = 1u;
static uint8_t kills = 0u, gold = 0u, sec_frames = 0u;   /* run stats for the end screens */
static uint16_t secs = 0u;
static void win_screen(void);
static void game_over(void);
static uint8_t hud_last_floor = 0xFFu;

#define WM(r,c) (*(uint8_t *)(0x9C00u + (r) * 32u + (c)))
#define BM(r,c) (*(uint8_t *)(0x9800u + (r) * 32u + (c)))

/* "FLOOR n" banner over view tile row 1 for BANNER_T frames after a level
 * loads, written by the VBL ISR (hud_update). L and R are not in the HUD
 * set: init_screen copies them from the font into free tiles 238-239. */
#define BANNER_T   90u                  /* 1.5 s */
#define BANNER_ROW 5u                   /* BG row = view tile row 1 */
#define BANNER_COL 5u                   /* 9 tiles, centred in view cols 2..17 */
#define T_L 238u
#define T_R 239u
static volatile uint8_t banner_t;       /* frames left; 0 = off */
static uint8_t banner_on;               /* banner tiles are in the BG map */

/* Intro run by the main loop after a floor or save loads (see intro()). */
#define INTRO_GLIDE 1u                  /* new floor: auto-walk down the corridor */
#define INTRO_SPIN  2u                  /* loaded save: one turn on the spot */
static uint8_t intro_req;

/* ---- enemies ---- */
#define NENEMY 8u
static uint16_t ex[NENEMY], ey[NENEMY];
static uint8_t ealive[NENEMY];
static uint8_t dmg_cd;
static uint8_t level_no, item_n, stairs_x, stairs_y;
static uint8_t item_x[8], item_y[8], item_taken[8], item_kind[8];   /* kind: item_art row */
#define IT_KEY    0u
#define IT_HEALTH 1u
#define IT_GOLD   2u
#define IT_MAP    3u
#define IT_STAIRS 4u
static uint8_t ehp[NENEMY], ehit[NENEMY];   /* enemy hits left, hit-flash ticks */
static uint8_t etype[NENEMY];               /* enemy_art row: 0 grunt, 1 bat, 2 brute */


#ifdef PROFILE
/* make PROFILE=1: per-stage scanlines (x456 = T) over the first PROF_N
 * images after prof_imgs is zeroed; read prof_acc[7] with read_memory.
 * Stages: 0 logic, 1 render_asm, 2 seen[], 3 flip wait, 4 copy,
 * 5 sprite_update, 6 gun + rest. Fixed seed so runs repeat. */
#define PROF_N 64u
uint32_t prof_acc[7];
uint8_t prof_imgs, prof_bfs;
static uint8_t prof_v, prof_l;
static void prof_mark(uint8_t s)
{
    uint8_t v, l;
    do { v = vbl_count; l = LY_REG; } while (v != vbl_count);
    l = (l >= 144u) ? (uint8_t)(l - 144u) : (uint8_t)(l + 10u);   /* lines since VBL IRQ */
    if (prof_imgs < PROF_N)
        prof_acc[s] += (uint16_t)((int16_t)((uint8_t)(v - prof_v) * 154u) + l - prof_l);
    prof_v = v;
    prof_l = l;
}
#define PROF(s) prof_mark(s)
#define PROF_BFS prof_bfs++
#else
#define PROF(s)
#define PROF_BFS
#endif

static uint8_t blocked(uint16_t x, uint16_t y);
#include "ai.h"                         /* needs imap, cam, ex/ey, blocked() */

static void load_level(uint8_t n)
{
    uint16_t i;
    uint8_t ne = 0u;
    const uint8_t *m = vbuf;

    /* new maze into vbuf (free until the next render); seen[] and the back
     * half of vbuf are the generator's scratch, both rewritten below/later */
    maze_generate(n, vbuf, seen, (uint16_t *)(vbuf + 1024u));

    level_no = n;
    floor_no = (uint8_t)(n + 1u);
    banner_t = BANNER_T;
    intro_req = INTRO_GLIDE;
    keys = 0u;
    item_n = 0u;
    for (i = 0u; i < 1024u; i++) {
        uint8_t ch = m[i];
        uint8_t v = 0u;
        if (ch == '#') v = 1u;
        else if (ch == '=') v = 2u;
        else if (ch == 'D') v = 3u;
        else if (ch == 'P') {
            cam_x = (uint16_t)(((uint16_t)(i & 31u) << 8) + 128u);
            cam_y = (uint16_t)(((uint16_t)(i >> 5) << 8) + 128u);
        } else if (ch == 'K' || ch == 'H' || ch == 'G' || ch == 'M') {
            if (item_n < 8u) {
                item_x[item_n] = (uint8_t)(i & 31u);
                item_y[item_n] = (uint8_t)(i >> 5);
                item_taken[item_n] = 0u;
                if (ch == 'K')      item_kind[item_n] = IT_KEY;
                else if (ch == 'H') item_kind[item_n] = IT_HEALTH;
                else if (ch == 'G') item_kind[item_n] = IT_GOLD;
                else                item_kind[item_n] = IT_MAP;
                item_n++;
            }
        } else if (ch == 'S') {
            stairs_x = (uint8_t)(i & 31u);
            stairs_y = (uint8_t)(i >> 5);
        } else if ((ch >= '1' && ch <= '8') || ch == 'B' || ch == 'X') {
            if (ne < NENEMY) {
                ex[ne] = (uint16_t)(((uint16_t)(i & 31u) << 8) + 128u);
                ey[ne] = (uint16_t)(((uint16_t)(i >> 5) << 8) + 128u);
                ealive[ne] = 1u;
                ehit[ne] = 0u;
                if (ch == 'B') {                 /* bat: fast, flies, one hit */
                    etype[ne] = 1u;
                    ehp[ne] = 1u;
                } else if (ch == 'X') {          /* brute: slow, big, three hits */
                    etype[ne] = 2u;
                    ehp[ne] = 3u;
                } else {
                    etype[ne] = 0u;
                    ehp[ne] = (n >= 2u) ? 2u : 1u;   /* floors 3-4: two hits */
                }
                ne++;
            }
        }
        imap[i] = v;
        seen[i] = 0u;
    }
    seen_at = 0xFFFFu;
    ai_reset();
    for (i = ne; i < NENEMY; i++) ealive[i] = 0u;
    /* face the first open direction: E, S, W, N */
    i = (uint16_t)(((cam_y >> 8) << 5) + (cam_x >> 8));
    if (!imap[i + 1u])       cam_a = 0u;
    else if (!imap[i + 32u]) cam_a = 64u;
    else if (!imap[i - 1u])  cam_a = 128u;
    else                     cam_a = 192u;
}

static uint8_t blocked(uint16_t x, uint16_t y)
{
    return imap[((y >> 8) << 5) + (x >> 8)];
}


static void move(int16_t dx, int16_t dy)
{
    uint16_t nx = (uint16_t)((int16_t)cam_x + dx);
    uint16_t ny = (uint16_t)((int16_t)cam_y + dy);
    uint16_t idx;
    uint8_t c;

    idx = (uint16_t)(((ny >> 8) << 5) + (nx >> 8));
    c = imap[idx];
    if (c == 3u) {
        if (keys) {
            keys--;
            imap[idx] = 0u;
            c = 0u;
            sfx_play(SFX_DOOR);
            ai_reset();                  /* new path through the door */
        }
    }
    if (!c) cam_x = nx;
    idx = (uint16_t)(((ny >> 8) << 5) + (cam_x >> 8));
    c = imap[idx];
    if (c == 3u) {
        if (keys) {
            keys--;
            imap[idx] = 0u;
            c = 0u;
            sfx_play(SFX_DOOR);
            ai_reset();                  /* new path through the door */
        }
    }
    if (!c) cam_y = ny;
}

/* Item pickups and stairs, checked after movement. Items are only
 * rescanned on a new cell or a hearts change (a health item left lying at
 * full hearts is taken once hurt); nothing else changes the outcome. */
static uint8_t pick_hp;
static void post_move(void)
{
    uint8_t cx = (uint8_t)(cam_x >> 8), cy = (uint8_t)(cam_y >> 8);
    uint8_t i;
    uint16_t at = ((uint16_t)cy << 5) + cx;

    if (at != seen_at) {                 /* new cell: reveal it and its 8 neighbours */
        seen_at = at;
        at -= 33u;
        for (i = 0u; i < 3u; i++) {
            seen[at] = 1u; seen[at + 1u] = 1u; seen[at + 2u] = 1u;
            at += 32u;
        }
        pick_hp = 0xFFu;                 /* force the item scan below */
    }

    if (hp != pick_hp)
    for (i = 0u; i < item_n; i++) {
        if (!item_taken[i]) {
            if (item_x[i] == cx) {
                if (item_y[i] == cy) {
                    uint8_t k = item_kind[i];
                    if (k == IT_HEALTH) {
                        if (hp >= HUD_MAX_HP) continue;   /* left lying if full */
                        hp++;
                    } else if (k == IT_KEY) {
                        keys++;
                    } else if (k == IT_GOLD) {
                        gold++;
                    } else {
                        memset(seen, 1, sizeof(seen));     /* map: whole floor on the automap */
                    }
                    item_taken[i] = 1u;
                    sfx_play(SFX_KEY);
                }
            }
        }
    }
    pick_hp = hp;
    if (stairs_x == cx) {
        if (stairs_y == cy) {
            uint8_t n = (uint8_t)(level_no + 1u);
            sfx_play(SFX_STAIRS);
            if (n >= NLEVELS) {          /* cleared the last floor */
                win_screen();
                n = 0u;
            }
            load_level(n);
        }
    }
}

/* Walk/strafe step per tick: dir * 16 / 128 = dir / 8 px (8.8), as a
 * shift: SDCC turns the int16 multiply into a slow library call. */
#define STEP(d) ((int16_t)((d) >> 3))

static void handle_input(void)
{
    uint8_t k = joypad();

    if (k & (J_LEFT | J_RIGHT)) {
        if (k & J_B) {                            /* strafe */
            int16_t sx = STEP(cos8[(uint8_t)(cam_a + 64u)]);
            int16_t sy = STEP(sin8[(uint8_t)(cam_a + 64u)]);
            if (k & J_LEFT)  move((int16_t)-sx, (int16_t)-sy);
            else             move(sx, sy);
        } else {                                  /* turn */
            if (k & J_LEFT)  cam_a -= 2u;
            else             cam_a += 2u;
        }
    }
    if (k & J_UP) move(STEP(cos8[cam_a]), STEP(sin8[cam_a]));
    if (k & J_DOWN) move((int16_t)-STEP(cos8[cam_a]), (int16_t)-STEP(sin8[cam_a]));
    /* gun sway while moving (one cycle ~0.7 s), snaps back at rest */
    if (k & (J_UP | J_DOWN)) gun_bob += 6u;
    else if ((k & J_B) && (k & (J_LEFT | J_RIGHT))) gun_bob += 6u;
    else gun_bob = 0u;
}


/* AI step for every live enemy (sdcccall 1): note contact (|cam - e| <
 * 200 on both axes, before the step), then count down a hit stun or step
 * with ai_sp = AI_SPEED (6), x2 for bats, /2 for brutes. Returns non-zero
 * if any enemy was in contact. Literals mirror NENEMY and AI_SPEED (the
 * preprocessor would mangle 8u in asm). */
static uint8_t enemy_pass(void) __naked
{
    __asm
    ld   bc, #0                 ; b = contact, c = enemy
1$: ld   hl, #_ealive
    ld   a, l
    add  a, c
    ld   l, a
    adc  a, h
    sub  a, l
    ld   h, a
    ld   a, (hl)
    or   a, a
    jp   z, 9$
    ld   a, c                   ; (uint16_t)(cam_x - ex[i] + 199) < 399
    add  a, a
    ld   e, a
    ld   d, #0
    ld   hl, #_ex
    add  hl, de
    ld   a, (_cam_x)
    sub  a, (hl)
    ld   e, a
    inc  hl
    ld   a, (_cam_x + 1)
    sbc  a, (hl)
    ld   d, a
    ld   hl, #199
    add  hl, de
    ld   a, h
    cp   a, #1
    jr   c, 2$                  ; < 256
    jr   nz, 5$
    ld   a, l
    cp   a, #0x8f               ; 399 = 0x18f
    jr   nc, 5$
2$: ld   a, c                   ; same for y
    add  a, a
    ld   e, a
    ld   d, #0
    ld   hl, #_ey
    add  hl, de
    ld   a, (_cam_y)
    sub  a, (hl)
    ld   e, a
    inc  hl
    ld   a, (_cam_y + 1)
    sbc  a, (hl)
    ld   d, a
    ld   hl, #199
    add  hl, de
    ld   a, h
    cp   a, #1
    jr   c, 4$
    jr   nz, 5$
    ld   a, l
    cp   a, #0x8f
    jr   nc, 5$
4$: ld   b, #1                  ; contact
5$: ld   hl, #_ehit
    ld   a, l
    add  a, c
    ld   l, a
    adc  a, h
    sub  a, l
    ld   h, a
    ld   a, (hl)
    or   a, a
    jr   z, 6$
    dec  (hl)                   ; stunned while flashing
    jr   9$
6$: ld   hl, #_etype            ; follow corridors toward the player
    ld   a, l
    add  a, c
    ld   l, a
    adc  a, h
    sub  a, l
    ld   h, a
    ld   a, (hl)
    ld   e, #6                  ; AI_SPEED
    cp   a, #1
    jr   nz, 7$
    ld   e, #12                 ; bats
7$: cp   a, #2
    jr   nz, 8$
    ld   e, #3                  ; brutes
8$: ld   a, e
    ld   (_ai_sp), a
    push bc
    ld   a, c
    call _ai_step
    pop  bc
9$: inc  c
    ld   a, c
    cp   a, #8                  ; NENEMY
    jp   c, 1$
    ld   a, b
    ret
    __endasm;
}

/* Contact damage is applied once after the pass (the old per-enemy loop
 * could only hurt once a tick too: dmg_cd blocks the rest). */
static void enemy_tick(void)
{
    ai_update();
    if (enemy_pass() && dmg_cd == 0u) {
        if (hp) hp--;
        dmg_cd = 60u;
        flash_pal = 0x1Bu; flash_n = 6u;          /* hurt: invert */
        sfx_play(SFX_HURT);
        if (hp == 0u) game_over();     /* loads the save or floor 1 */
    }
    if (dmg_cd) dmg_cd--;
}


#define MAX_SPR    40u
#define BAT_Y      68u                  /* screen y of a bat sprite's bottom: just above eye level */

/* Project a world point (8.8) with the current camera. Returns 1 when it is
 * in range, inside the 90 deg FOV and nearer than the wall at its column
 * (r_zb, 1/8 cells). Sets p_fwd (8.8 depth), p_x (screen px) and p_foot
 * (screen y of the floor under it). */
int16_t p_fwd;                          /* project() results (src/math.s) */
uint8_t p_x, p_foot;

uint8_t project(uint16_t wx, uint16_t wy);   /* src/math.s */

/* foot_tab[k] = min(HORIZON + 256/k, VIEW_Y0 + 64), k = fwd >> 5 in 3..80 */
const uint8_t foot_tab[81] = {
    96u, 96u, 96u, 96u, 96u, 96u, 96u, 96u, 96u, 92u, 89u, 87u, 85u, 83u, 82u, 81u,
    80u, 79u, 78u, 77u, 76u, 76u, 75u, 75u, 74u, 74u, 73u, 73u, 73u, 72u, 72u, 72u,
    72u, 71u, 71u, 71u, 71u, 70u, 70u, 70u, 70u, 70u, 70u, 69u, 69u, 69u, 69u, 69u,
    69u, 69u, 69u, 69u, 68u, 68u, 68u, 68u, 68u, 68u, 68u, 68u, 68u, 68u, 68u, 68u,
    68u, 67u, 67u, 67u, 67u, 67u, 67u, 67u, 67u, 67u, 67u, 67u, 67u, 67u, 67u, 67u,
    67u
};

static uint8_t enemy_tier(int16_t fwd)
{
    if (fwd < 384) return 2u;
    if (fwd < 768) return 1u;
    return 0u;
}

/* Items are about 1/4 cell: ~16 px nearer than 2 cells, ~8 px to 4, then ~4. */
static uint8_t item_tier(int16_t fwd)
{
    if (fwd < 512) return 2u;
    if (fwd < 1024) return 1u;
    return 0u;
}

/* One billboard from sprites.h art {w, h, tile|flip...}, bottom centre at
 * screen (x, y). Returns the next free OAM slot. src/billboard.s. */
uint8_t draw_art(uint8_t si, const uint8_t *a, uint8_t x, uint8_t y, uint8_t pal);

/* A: hitscan the nearest visible enemy overlapping the screen centre. */
static void shoot(void)
{
    uint8_t i, best = 0xFFu, d;
    int16_t best_fwd = 0x7FFF;

    flash_pal = 0x90u; flash_n = 2u;                 /* muzzle flash: lighten */
    sfx_play(SFX_SHOOT);
    for (i = 0u; i < NENEMY; i++) {
        if (!ealive[i]) continue;
        if (!project(ex[i], ey[i])) continue;
        d = (p_x > 80u) ? (uint8_t)(p_x - 80u) : (uint8_t)(80u - p_x);
        if (d > (uint8_t)(enemy_art[etype[i]][enemy_tier(p_fwd)][0] << 2)) continue;   /* half width */
        if (p_fwd < best_fwd) { best_fwd = p_fwd; best = i; }
    }
    if (best != 0xFFu) {
        ehp[best]--;
        if (ehp[best] == 0u) {
            ealive[best] = 0u;
            kills++;
            sfx_play(SFX_KILL);
        } else {
            ehit[best] = 3u;             /* 3 AI ticks: flash white, stunned */
            sfx_play(SFX_HIT);
        }
    }
}

/* DMG shows the first 10 OBJ of each scanline in OAM order. The gun is
 * slots 0-5 (3 per line); billboards share the other 7 by width, so a
 * whole far billboard is skipped rather than a near one losing columns.
 * Conservative: widths count as if every billboard shared one line. */
#define SPR_LINE 7u
static uint8_t item_ahead;              /* an item is drawn behind the gun: lower it */
static uint8_t spr_used = MAX_SPR;      /* OAM slots sprite_update may have shown */
static uint8_t gun_low, gun_low_shown;  /* lowered by px (0..16): target, in VRAM */

/* Lowered gun: the idle art slides down d rows inside its own 6 tiles
 * (gun.h order: top row pairs 0-2, bottom row 3-5), rows past the bottom
 * drop off, so nothing spills onto the frame below the view. STAT-safe
 * copies (GBDK set_sprite_data waits per byte), 192 bytes per change. */
static void gun_shift(uint8_t d)
{
    static uint8_t col[64];             /* one 8x32 column, 2 bytes per row */
    uint8_t c, k, n = (uint8_t)(d << 1), t;
    const uint8_t *top, *bot;

    for (c = 0u; c < 3u; c++) {
        top = &gun_tiles[(uint16_t)c << 5];
        bot = &gun_tiles[(uint16_t)(c + 3u) << 5];
        for (k = 0u; k < 64u; k++) {
            if (k < n) { col[k] = 0u; continue; }
            t = (uint8_t)(k - n);
            if (t < 32u) col[k] = top[t];
            else         col[k] = bot[(uint8_t)(t - 32u)];
        }
        set_sprite_data((uint8_t)(GUN_T0 + (c << 1)), 2u, col);
        set_sprite_data((uint8_t)(GUN_T0 + ((c + 3u) << 1)), 2u, col + 32u);
    }
}

/* Item art at the last project(), if it fits the line budget. */
static uint8_t draw_item(uint8_t si, uint8_t kind, uint8_t *room)
{
    const uint8_t *a = item_art[kind][item_tier(p_fwd)];

    if (a[0] > *room) return si;
    *room -= a[0];
    if (p_x > 64u && p_x < 104u)        /* overlaps the gun (screen x 72-96, top 72) */
        if (p_foot > 72u) item_ahead = 1u;
    return draw_art(si, a, p_x, p_foot, S_PALETTE);
}

/* project() results per visible object, filled by the scans below. The
 * asm relies on this layout: k, x, foot, fwd at +0/+9/+18/+27. */
typedef struct {
    uint8_t k[9];                        /* enemy index, or item_art kind */
    uint8_t x[9], foot[9];
    int16_t fwd[9];
} vis_t;
static vis_t ven;                        /* enemies (up to NENEMY) */
static vis_t vit;                        /* items (up to 8), then stairs */

/* Store the last project() result as entry B (then b++) of the vis_t at
 * DE, with A as its k byte. Clobbers HL, DE. */
static void scan_store(void) __naked
{
    __asm
    ld   l, b
    ld   h, #0
    add  hl, de                 ; &kind[b]
    ld   (hl), a
    ld   de, #9
    add  hl, de                 ; &x[b]
    ld   a, (_p_x)
    ld   (hl), a
    add  hl, de                 ; &foot[b]
    ld   a, (_p_foot)
    ld   (hl), a
    add  hl, de                 ; &fwd + b (bytes)
    ld   e, b
    add  hl, de                 ; &fwd[b] (int16)
    ld   a, (_p_fwd)
    ld   (hl+), a
    ld   a, (_p_fwd + 1)
    ld   (hl), a
    inc  b
    ret
    __endasm;
}

/* Visible live enemies into ven; returns how many. Same
 * project() calls as the old C loop, without SDCC's int16 indexing. */
static uint8_t scan_enemies(void) __naked
{
    __asm
    ld   bc, #0                 ; b = visible, c = enemy
1$: ld   hl, #_ealive
    ld   a, l
    add  a, c
    ld   l, a
    adc  a, h
    sub  a, l
    ld   h, a
    ld   a, (hl)
    or   a, a
    jr   z, 2$
    push bc
    ld   a, c
    add  a, a
    ld   e, a
    ld   d, #0
    ld   hl, #_ey
    add  hl, de
    ld   a, (hl+)
    ld   b, (hl)
    ld   c, a                   ; bc = ey[i]
    ld   hl, #_ex
    add  hl, de
    ld   a, (hl+)
    ld   d, (hl)
    ld   e, a                   ; de = ex[i]
    call _project
    pop  bc
    or   a, a
    jr   z, 2$
    ld   de, #_ven
    ld   a, c
    call _scan_store
2$: inc  c
    ld   a, c
    cp   a, #8                  ; NENEMY
    jr   c, 1$
    ld   a, b
    ret
    __endasm;
}

/* Visible untaken items (cell centres), then the stairs, into vit with
 * their item_art kind; returns how many. */
static uint8_t scan_items(void) __naked
{
    __asm
    ld   bc, #0                 ; b = visible, c = item
1$: ld   a, (_item_n)
    cp   a, c
    jr   z, 3$
    ld   hl, #_item_taken
    ld   a, l
    add  a, c
    ld   l, a
    adc  a, h
    sub  a, l
    ld   h, a
    ld   a, (hl)
    or   a, a
    jr   nz, 2$
    push bc
    ld   hl, #_item_y
    ld   a, l
    add  a, c
    ld   l, a
    adc  a, h
    sub  a, l
    ld   h, a
    ld   b, (hl)
    ld   hl, #_item_x
    ld   a, l
    add  a, c
    ld   l, a
    adc  a, h
    sub  a, l
    ld   h, a
    ld   d, (hl)
    ld   e, #128
    ld   c, e                   ; (x << 8) + 128, (y << 8) + 128
    call _project
    pop  bc
    or   a, a
    jr   z, 2$
    ld   hl, #_item_kind
    ld   a, l
    add  a, c
    ld   l, a
    adc  a, h
    sub  a, l
    ld   h, a
    ld   a, (hl)
    ld   de, #_vit
    call _scan_store
2$: inc  c
    jr   1$
3$: push bc
    ld   a, (_stairs_x)
    ld   d, a
    ld   e, #128
    ld   a, (_stairs_y)
    ld   b, a
    ld   c, e
    call _project
    pop  bc
    or   a, a
    jr   z, 4$
    ld   de, #_vit
    ld   a, #4                  ; IT_STAIRS
    call _scan_store
4$: ld   a, b
    ret
    __endasm;
}

/* Billboards for the camera the view was just rendered from: up to 3
 * enemies nearest first, then items and stairs. */
static void sprite_update(void)
{
    static const int8_t bob[8] = {0, 1, 2, 1, 0, -1, -2, -1};
    uint8_t si = GUN_SPRITES, room = SPR_LINE, n, k, j, best, i, y, pal;   /* 0.. = gun */
    const uint8_t *a;

    n = scan_enemies();
    for (k = 0u; k < 3u; k++) {
        best = 0xFFu;
        p_fwd = 0x7FFF;                                 /* project() caps fwd at 2560 */
        for (j = 0u; j < n; j++)
            if (ven.fwd[j] < p_fwd) { p_fwd = ven.fwd[j]; best = j; }
        if (best == 0xFFu) break;
        ven.fwd[best] = 0x7FFF;                         /* taken */
        i = ven.k[best];
        a = enemy_art[etype[i]][enemy_tier(p_fwd)];
        if (a[0] > room) continue;
        room -= a[0];
        pal = 0u;
        if (ehit[i]) pal = S_PALETTE;
        y = ven.foot[best];                             /* grunts stand on the floor */
        if (etype[i] == 1u) y = (uint8_t)(BAT_Y + bob[(uint8_t)((vbl_count >> 3) + i) & 7u]);
        si = draw_art(si, a, ven.x[best], y, pal);
    }
    /* items and stairs stand on the floor; OBP1 so colour 3 shows white */
    item_ahead = 0u;
    n = scan_items();
    for (j = 0u; j < n; j++) {
        p_x = vit.x[j]; p_foot = vit.foot[j]; p_fwd = vit.fwd[j];
        si = draw_item(si, vit.k[j], &room);
    }
    for (i = si; i < spr_used; i++) move_sprite(i, 0u, 0u);   /* the rest are hidden */
    spr_used = si;
}

/* ---- automap (Select): explored cells, 2x2 px each, 64x64 in the middle
 * of the viewport; the player is an 8-direction arrow sprite. ---- */
#define SPR_ARROW 154u                   /* one pair, loaded per map from 8 dirs x (arrow, blank) */
#define MAP_X0    32u                    /* map left edge inside vbuf, px */
const uint8_t arrow_tiles[16 * 16] = {
    0x00,0x00, 0x10,0x10, 0x08,0x08, 0x7C,0x7C, 0x08,0x08, 0x10,0x10, 0x00,0x00, 0x00,0x00,
    0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0,
    0x00,0x00, 0x40,0x40, 0x20,0x20, 0x14,0x14, 0x0C,0x0C, 0x1C,0x1C, 0x00,0x00, 0x00,0x00,
    0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0,
    0x00,0x00, 0x10,0x10, 0x10,0x10, 0x54,0x54, 0x38,0x38, 0x10,0x10, 0x00,0x00, 0x00,0x00,
    0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0,
    0x00,0x00, 0x04,0x04, 0x08,0x08, 0x50,0x50, 0x60,0x60, 0x70,0x70, 0x00,0x00, 0x00,0x00,
    0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0,
    0x00,0x00, 0x10,0x10, 0x20,0x20, 0x7C,0x7C, 0x20,0x20, 0x10,0x10, 0x00,0x00, 0x00,0x00,
    0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0,
    0x00,0x00, 0x70,0x70, 0x60,0x60, 0x50,0x50, 0x08,0x08, 0x04,0x04, 0x00,0x00, 0x00,0x00,
    0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0,
    0x00,0x00, 0x10,0x10, 0x38,0x38, 0x54,0x54, 0x10,0x10, 0x10,0x10, 0x00,0x00, 0x00,0x00,
    0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0,
    0x00,0x00, 0x1C,0x1C, 0x0C,0x0C, 0x14,0x14, 0x20,0x20, 0x40,0x40, 0x00,0x00, 0x00,0x00,
    0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0,
};

static void init_enemies(void)
{
    sprites_init();
    gun_init();
    OBP0_REG = 0xE4u;
    OBP1_REG = 0x24u;                    /* colour 3 -> white, 2 -> light */
    for (uint8_t j = 0; j < MAX_SPR; j++) move_sprite(j, 0u, 0u);
}
/* Paint one map cell (2x2 px) colour c into vbuf. */
static void plot_cell(uint8_t r, uint8_t c, uint8_t col)
{
    uint8_t px = (uint8_t)(MAP_X0 + (c << 1));
    uint8_t m = (uint8_t)(0xC0u >> (px & 7u));
    uint8_t lo = (col & 1u) ? m : 0u, hi = (col & 2u) ? m : 0u;
    uint8_t *p = &vbuf[((uint16_t)(px >> 3) << 7) + ((uint16_t)r << 2)];
    m = (uint8_t)~m;
    p[0] = (uint8_t)((p[0] & m) | lo); p[1] = (uint8_t)((p[1] & m) | hi);
    p[2] = (uint8_t)((p[2] & m) | lo); p[3] = (uint8_t)((p[3] & m) | hi);
}

/* Colours: unseen black, floor dark, wall light; door, keys, stairs white. */
static void draw_map(void)
{
    uint16_t i;
    uint8_t r, c;

    for (i = 0u; i < 2048u; i++) vbuf[i] = 0xFFu;
    for (r = 0u; r < 32u; r++) {
        for (c = 0u; c < 32u; c++) {
            i = ((uint16_t)r << 5) + c;
            if (!seen[i]) continue;
            if (imap[i] == 0u) plot_cell(r, c, 2u);
            else if (imap[i] == 3u) plot_cell(r, c, 0u);
            else plot_cell(r, c, 1u);
        }
    }
    for (c = 0u; c < item_n; c++) {
        if (item_taken[c]) continue;
        if (seen[((uint16_t)item_y[c] << 5) + item_x[c]]) plot_cell(item_y[c], item_x[c], 0u);
    }
    if (seen[((uint16_t)stairs_y << 5) + stairs_x]) plot_cell(stairs_y, stairs_x, 0u);
}

/* Forget both banks' spans and column keys: the next two copy_view calls
 * copy everything. Needed at boot and whenever something else writes view
 * tiles 0..127. */
static void view_reset(void)
{
    uint8_t i;

    for (i = 0u; i < 32u; i++) {
        r_vh[i] = 0u; r_vh[i + 32u] = 64u;
        c_key[(uint16_t)i << 4] = 0xFFu;   /* tops are <= 31: never matches */
    }
}

static uint8_t hud_dir = 0xFFu;

static void init_hud(void)
{
    uint8_t row[20];

    for (uint8_t c = 0; c < 20u; c++) row[c] = T_WHITE;
    for (uint8_t c = 0; c < HUD_MAX_HP; c++)
        row[c] = (c < hp) ? HUD_HEART_FULL : HUD_HEART_EMPTY;
    row[5] = HUD_K;
    row[6] = HUD_DIGIT(keys);
    row[7] = HUD_F;
    row[8] = HUD_DIGIT(floor_no);
    row[10] = HUD_GOLD;
    row[11] = HUD_DIGIT(gold / 10u);
    row[12] = HUD_DIGIT(gold % 10u);
    row[13] = HUD_N;
    row[15] = HUD_E;
    row[17] = HUD_S;
    row[19] = HUD_W;
    set_win_tiles(0u, 0u, 20u, 1u, row);
}

/* Called right after wait_vbl_done (still VBlank). */
void hud_update(void)                    /* called from the VBL ISR */
{
    static const uint8_t cell[4] = {15u, 17u, 19u, 13u};      /* E S W N */
    static const uint8_t norm[4] = {HUD_E, HUD_S, HUD_W, HUD_N};
    static const uint8_t inv[4]  = {HUD_INV_E, HUD_INV_S, HUD_INV_W, HUD_INV_N};
    static uint8_t last_hp = 0xFFu, last_keys = 0xFFu, last_gold = 0xFFu;
    uint8_t d = (uint8_t)(((cam_a + 32u) >> 6) & 3u);

    if (d != hud_dir) {
        if (hud_dir != 0xFFu) WM(0, cell[hud_dir]) = norm[hud_dir];
        WM(0, cell[d]) = inv[d];
        hud_dir = d;
    }
    if (hp != last_hp) {
        for (uint8_t c = 0; c < HUD_MAX_HP; c++)
            WM(0, c) = (c < hp) ? HUD_HEART_FULL : HUD_HEART_EMPTY;
        last_hp = hp;
    }
    if (keys != last_keys) {
        WM(0, 6) = HUD_DIGIT(keys);
        last_keys = keys;
    }
    if (gold != last_gold) {
        WM(0, 11) = HUD_DIGIT(gold / 10u);
        WM(0, 12) = HUD_DIGIT(gold % 10u);
        last_gold = gold;
    }
    if (floor_no != hud_last_floor) {
        WM(0, 8) = HUD_DIGIT(floor_no);
        hud_last_floor = floor_no;
    }
    if (banner_t) {                      /* " FLOOR n " in HUD glyphs on white */
        BM(BANNER_ROW, BANNER_COL)      = T_WHITE;
        BM(BANNER_ROW, BANNER_COL + 1u) = HUD_F;
        BM(BANNER_ROW, BANNER_COL + 2u) = T_L;
        BM(BANNER_ROW, BANNER_COL + 3u) = HUD_DIGIT(0);
        BM(BANNER_ROW, BANNER_COL + 4u) = HUD_DIGIT(0);
        BM(BANNER_ROW, BANNER_COL + 5u) = T_R;
        BM(BANNER_ROW, BANNER_COL + 6u) = T_WHITE;
        BM(BANNER_ROW, BANNER_COL + 7u) = HUD_DIGIT(floor_no);
        BM(BANNER_ROW, BANNER_COL + 8u) = T_WHITE;
        banner_on = 1u;
        banner_t--;
    } else if (banner_on) {              /* view tiles back: index = col * 8 + row */
        for (uint8_t c = BANNER_COL; c < BANNER_COL + 9u; c++)
            BM(BANNER_ROW, c) = (uint8_t)(((c - 2u) << 3) + (BANNER_ROW - 4u));
        banner_on = 0u;
    }
}


/* Extra letters for the title/end screens, HUD style (3x5, black on white).
 * O reuses digit 0; E S N W K F come from the HUD set. Loaded only while a
 * text screen or the map is up: in play these tiles hold sprites (sprites.h). */
#define FONT_BASE 184u
#define FONT_N    15u
const uint8_t font_tiles[FONT_N * 16] = {
    0,0, 0,0, 40,40, 40,40, 16,16, 16,16, 16,16, 0,0,   /* Y */
    0,0, 0,0, 40,40, 40,40, 40,40, 40,40, 56,56, 0,0,   /* U */
    0,0, 0,0, 56,56, 32,32, 32,32, 32,32, 56,56, 0,0,   /* C */
    0,0, 0,0, 56,56, 40,40, 56,56, 40,40, 40,40, 0,0,   /* A */
    0,0, 0,0, 56,56, 40,40, 56,56, 32,32, 32,32, 0,0,   /* P */
    0,0, 0,0, 48,48, 40,40, 40,40, 40,40, 48,48, 0,0,   /* D */
    0,0, 0,0, 56,56, 16,16, 16,16, 16,16, 16,16, 0,0,   /* T */
    0,0, 0,0, 56,56, 16,16, 16,16, 16,16, 56,56, 0,0,   /* I */
    0,0, 0,0, 32,32, 32,32, 32,32, 32,32, 56,56, 0,0,   /* L */
    0,0, 0,0, 56,56, 40,40, 48,48, 40,40, 40,40, 0,0,   /* R */
    0,0, 0,0, 56,56, 32,32, 40,40, 40,40, 56,56, 0,0,   /* G */
    0,0, 0,0, 68,68, 108,108, 84,84, 68,68, 68,68, 0,0, /* M (5 wide, else it reads as N) */
    0,0, 0,0, 40,40, 40,40, 40,40, 40,40, 16,16, 0,0,   /* V */
    0,0, 0,0, 48,48, 40,40, 48,48, 40,40, 48,48, 0,0,   /* B */
    0,0, 0,0, 56,56, 8,8, 16,16, 32,32, 56,56, 0,0,     /* Z */
};

/* HUD gem for the gold count (BG tile 183) */
const uint8_t gold_tile[16] = {
    0,0, 0x7C,0x7C, 0xD6,0xD6, 0xFE,0xFE, 0x6C,0x6C, 0x38,0x38, 0x10,0x10, 0,0,
};

static uint8_t glyph(char ch)
{
    static const char extra[] = "YUCAPDTILRGMVBZ";
    static const char hud[] = "NESWKF";
    uint8_t i;

    if (ch >= '0' && ch <= '9') return HUD_DIGIT(ch - '0');
    if (ch == 'O') return HUD_DIGIT(0);
    for (i = 0u; i < FONT_N; i++) if (extra[i] == ch) return (uint8_t)(FONT_BASE + i);
    for (i = 0u; i < 6u; i++) if (hud[i] == ch) return (uint8_t)(HUD_N + i);
    return T_WHITE;
}

/* Black border with the viewport tiles column-major at (2..17, 4..11). */
static void draw_frame(void)
{
    uint8_t row[20];

    for (uint8_t c = 0; c < 20u; c++) row[c] = T_BLACK;
    for (uint8_t y = 0; y < 18u; y++) set_bkg_tiles(0u, y, 20u, 1u, row);
    for (uint8_t tr = 0; tr < 8u; tr++) {
        for (uint8_t tc = 0; tc < 16u; tc++) row[tc] = (uint8_t)(tc * 8u + tr);
        set_bkg_tiles(2u, (uint8_t)(4u + tr), 16u, 1u, row);
    }
    frame_draw();
}

/* One centred white banner line of text on BG row y. */
static void banner(uint8_t y, const char *t)
{
    uint8_t row[20], n = 0u, c;

    while (t[n]) n++;
    for (c = 0u; c < 20u; c++) row[c] = T_BLACK;
    if (n)
        for (c = 0u; c < (uint8_t)(n + 2u); c++) row[(uint8_t)(9u - (n >> 1) + c)] = T_WHITE;
    for (c = 0u; c < n; c++) row[(uint8_t)(10u - (n >> 1) + c)] = glyph(t[c]);
    set_bkg_tiles(0u, y, 20u, 1u, row);
}

/* ---- save: battery SRAM (MBC1+RAM+BATTERY, Makefile), 3 slots of
 * SLOT_SIZE from 0xA000. Each: magic, the floor (for the slot picker),
 * then every variable below in order. Change the list or any size -> bump
 * the magic, so old saves read as empty. ---- */
#define SRAM      ((uint8_t *)0xA000u)
#define NSLOTS    3u
#define SLOT_SIZE 0x0A00u                /* > 5 + sum of save_vars (~2.2 KB) */
#define SLOT_HDR  5u                     /* magic + floor */
static const uint8_t save_magic[4] = {'D', 'G', 'N', '2'};
static const struct { void *p; uint16_t n; } save_vars[] = {
    {&cam_x, 2u}, {&cam_y, 2u}, {&cam_a, 1u},
    {&hp, 1u}, {&keys, 1u}, {&level_no, 1u}, {&kills, 1u}, {&gold, 1u}, {&secs, 2u},
    {ex, sizeof ex}, {ey, sizeof ey}, {ealive, sizeof ealive},
    {ehp, sizeof ehp}, {etype, sizeof etype},
    {&item_n, 1u}, {item_x, sizeof item_x}, {item_y, sizeof item_y},
    {item_taken, sizeof item_taken}, {item_kind, sizeof item_kind},
    {&stairs_x, 1u}, {&stairs_y, 1u},
    {imap, sizeof imap}, {seen, sizeof seen},
};
static uint8_t cur_slot;                 /* last loaded/saved: the picker starts here */

/* Floor saved in slot s (1..4), 0 = empty. */
static uint8_t slot_floor(uint8_t s)
{
    uint8_t *b = SRAM + s * SLOT_SIZE, f = 0u;

    ENABLE_RAM;
    if (memcmp(b, save_magic, 4u) == 0) f = b[4];
    DISABLE_RAM;
    return f;
}

static uint8_t save_ok(void)             /* any slot used */
{
    uint8_t s;

    for (s = 0u; s < NSLOTS; s++) if (slot_floor(s)) return 1u;
    return 0u;
}

/* store 1: game -> slot, 0: slot -> game. Magic is cleared before a store
 * and written last, so a power cut mid-save leaves an empty slot, not a bad one. */
static void save_copy(uint8_t slot, uint8_t store)
{
    uint8_t *b = SRAM + slot * SLOT_SIZE, *s = b + SLOT_HDR, i;

    ENABLE_RAM;
    if (store) b[0] = 0u;
    for (i = 0u; i < sizeof save_vars / sizeof save_vars[0]; i++) {
        if (store) memcpy(s, save_vars[i].p, save_vars[i].n);
        else       memcpy(save_vars[i].p, s, save_vars[i].n);
        s += save_vars[i].n;
    }
    if (store) {
        b[4] = floor_no;
        memcpy(b, save_magic, 4u);
    }
    DISABLE_RAM;
}

static void save_game(void) { save_copy(cur_slot, 1u); }

static void load_game(void)
{
    save_copy(cur_slot, 0u);
    floor_no = (uint8_t)(level_no + 1u);
    for (uint8_t i = 0u; i < NENEMY; i++) ehit[i] = 0u;
    seen_at = 0xFFFFu;
    ai_reset();
    banner_t = BANNER_T;
    intro_req = INTRO_SPIN;
}

static void label_num(char *buf, const char *label, uint16_t v);

/* Slot picker on BG row y ("SLOT 2 FLOOR 3" / "SLOT 3 EMPTY"), "PICK SLOT"
 * on row y-1. Left/right cycle, A/Start pick, B cancels. used_only skips
 * empty slots (loading; at least one must be used). Returns the slot or
 * 0xFF. The caller redraws both rows. */
static uint8_t pick_slot(uint8_t y, uint8_t used_only)
{
    char t[16];
    uint8_t s = cur_slot, k, f, i;

    banner((uint8_t)(y - 1u), "PICK SLOT");
    for (;;) {
        while (used_only && !slot_floor(s)) s = (uint8_t)((s + 1u) % NSLOTS);
        f = slot_floor(s);
        label_num(t, "SLOT", (uint8_t)(s + 1u));
        for (i = 0u; t[i]; i++) ;
        if (f) label_num(t + i, " FLOOR", f);
        else   strcpy(t + i, " EMPTY");
        banner(y, t);
        while (joypad()) wait_vbl_done();
        while (!(k = (uint8_t)(joypad() & (J_LEFT | J_RIGHT | J_A | J_START | J_B)))) wait_vbl_done();
        if (k & J_B) s = 0xFFu;
        if (k & (J_B | J_A | J_START)) break;
        do {
            s = (uint8_t)((s + ((k & J_RIGHT) ? 1u : NSLOTS - 1u)) % NSLOTS);
        } while (used_only && !slot_floor(s));
    }
    while (joypad()) wait_vbl_done();
    return s;
}

static void show_view(void);
static void att_step(void);
static uint8_t ticks(void);

/* Wait for Start, or A (can_load) and a slot pick on BG row `row` ("PRESS
 * START" / "A LOAD SAVE" sit on row-1 / row and come back on a cancel).
 * anim: the title walker keeps running meanwhile. Returns non-zero when a
 * slot was picked (cur_slot). */
static uint8_t start_or_load(uint8_t can_load, uint8_t row, uint8_t anim)
{
    uint8_t k = can_load ? (J_START | J_A) : J_START, pad, s, n;

    while (joypad() & k) wait_vbl_done();
    for (;;) {
        while (!((pad = joypad()) & k)) {
            if (anim) {
                for (n = ticks(); n; n--) att_step();
                show_view();
                flip_req = 1u;
                images++;
            } else {
                wait_vbl_done();
            }
        }
        if (!(pad & J_A)) break;
        s = pick_slot(row, 1u);
        if (s != 0xFFu) { cur_slot = s; break; }
        banner((uint8_t)(row - 1u), "PRESS START");
        banner(row, "A LOAD SAVE");
    }
    while (joypad() & k) wait_vbl_done();
    return (uint8_t)(pad & J_A);
}

/* Paused overhead map until Select (or A/Start) is pressed again; B toggles music. */
static void show_map(void)
{
    uint8_t si;

    draw_map();
    while (flip_req) ;                   /* hidden bank settled */
    r_dst = (LCDC_REG & LCDCF_BG8000) ? 0x9000u : 0x8000u;
    copy_vbuf();
    view_reset();                        /* the map replaced that bank's view */
    /* arrow centre is pixel (3,3) of its tile; OAM = screen + (8, 16) */
    set_sprite_data(SPR_ARROW, 2u, &arrow_tiles[(uint16_t)(((uint8_t)(cam_a + 16u)) >> 5) << 5]);   /* STAT-safe */
    set_sprite_tile(0u, SPR_ARROW);
    set_sprite_prop(0u, S_PALETTE);      /* white */
    move_sprite(0u, (uint8_t)(VIEW_X0 + MAP_X0 + (cam_x >> 7) - 3u + 8u),
                    (uint8_t)(VIEW_Y0 + (cam_y >> 7) - 3u + 16u));
    for (si = 1u; si < MAX_SPR; si++) move_sprite(si, 0u, 0u);
    gun_shown = 0xFFu;                   /* the arrow took slot 0 */
    flip_req = 1u;
    while (flip_req) ;

    set_bkg_data(FONT_BASE, FONT_N, font_tiles);   /* over sprite tiles: all hidden but the arrow */
    banner(15u, "START SAVE");

    while (joypad() & J_SELECT) wait_vbl_done();
    for (;;) {                           /* B toggles music, Start saves, Select/A close */
        banner(13u, "PRESS B");          /* also after the slot picker's hint */
        banner(14u, (NR51_REG & 0x02u) ? "MUSIC ON" : "MUSIC OFF");
        while (!(joypad() & (J_SELECT | J_START | J_A | J_B))) wait_vbl_done();
        si = joypad();
        if (si & J_START) {
            si = pick_slot(15u, 0u);
            if (si != 0xFFu) {
                char t[10];
                cur_slot = si;
                save_game();
                label_num(t, "SAVED", (uint8_t)(si + 1u));
                banner(15u, t);
            } else {
                banner(15u, "START SAVE");
            }
        } else if (si & J_B) {
            NR51_REG ^= 0x66u;           /* ch2 + ch3 (music.h) off both sides */
        } else break;
        while (joypad() & (J_START | J_B)) wait_vbl_done();
    }
    while (joypad() & (J_SELECT | J_A)) wait_vbl_done();
    frame_draw();                        /* bricks back over the text rows */
    set_sprite_data(FONT_BASE, FONT_N, spr_tiles + 64u);   /* sprites_init's 184.. block */
}

/* Writes v right after a label into buf: "KILLS 12". */
static void label_num(char *buf, const char *label, uint16_t v)
{
    char d[5];
    uint8_t n = 0u, i = 0u;

    while (label[i]) { buf[i] = label[i]; i++; }
    buf[i++] = ' ';
    do { d[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 5u);
    while (n) buf[i++] = d[--n];
    buf[i] = 0;
}

/* Full-screen text (4 lines + PRESS START) until Start is pressed, then
 * the normal frame and sprite tiles come back. can_load: A picks a save
 * slot ("A LOAD SAVE"); returns non-zero if one was picked (cur_slot). */
static uint8_t text_screen(const char *l1, const char *l2, const char *l3, const char *l4,
                           uint8_t can_load)
{
    uint8_t i;

    for (i = 0u; i < MAX_SPR; i++) move_sprite(i, 0u, 0u);
    gun_shown = 0xFFu;
    banner_t = 0u;                       /* the ISR must not touch the text */
    banner_on = 0u;
    DISPLAY_OFF;
    set_bkg_data(FONT_BASE, FONT_N, font_tiles);
    for (i = 0u; i < 18u; i++) banner(i, "");
    banner(4u, l1);
    banner(7u, l2);
    banner(9u, l3);
    banner(11u, l4);
    banner(14u, "PRESS START");
    if (can_load) banner(15u, "A LOAD SAVE");
    DISPLAY_ON;

    i = start_or_load(can_load, 15u, 0u);

    DISPLAY_OFF;
    draw_frame();
    sprites_init();                      /* the font overwrote some */
    DISPLAY_ON;
    return i;
}

static void new_run(void)
{
    hp = HUD_MAX_HP;
    kills = 0u;
    gold = 0u;
    secs = 0u;
}

/* Cleared floor 4: stats, then a fresh run (caller loads floor 1). */
static void win_screen(void)
{
    char a[12], b[12], c[12];

    label_num(a, "KILLS", kills);
    label_num(b, "SECS", secs);
    label_num(c, "GOLD", gold);
    text_screen("YOU ESCAPED", a, b, c, 0u);
    new_run();
}

/* Out of hearts: stats, then the save (A) or a fresh run on floor 1. */
static void game_over(void)
{
    char a[12], b[12], c[12];

    label_num(a, "FLOOR", floor_no);
    label_num(b, "KILLS", kills);
    label_num(c, "GOLD", gold);
    if (text_screen("GAME OVER", a, b, c, save_ok())) {
        load_game();
    } else {
        new_run();
        load_level(0u);
    }
}

static void init_screen(void)
{
    uint8_t row[20];

    DISPLAY_OFF;
    LCDC_REG = LCDCF_BG8000 | LCDCF_BG9800 | LCDCF_WIN9C00 | LCDCF_WINON
             | LCDCF_BGON | LCDCF_OBJ16 | LCDCF_OBJON;
    WY_REG = 128u; WX_REG = 7u; SCY_REG = 0u; SCX_REG = 0u;
    BGP_REG = 0xE4u;
    set_bkg_data(T_BLACK, 2u, tiles[0]);
    set_bkg_data(HUD_BASE, 23u, hud_tiles);
    set_bkg_data(HUD_GOLD, 1u, gold_tile);
    set_bkg_data(T_L, 1u, &font_tiles[(uint16_t)(glyph('L') - FONT_BASE) << 4]);
    set_bkg_data(T_R, 1u, &font_tiles[(uint16_t)(glyph('R') - FONT_BASE) << 4]);
    frame_init();
    draw_frame();

    for (uint8_t c = 0; c < 20u; c++) row[c] = T_WHITE;
    set_win_tiles(0u, 0u, 20u, 1u, row);
    row[9]  = HUD_DIGIT(BUILD / 10u);  /* build number */
    row[10] = HUD_DIGIT(BUILD % 10u);
    set_win_tiles(0u, 1u, 20u, 1u, row);
}

/* Camera plane rays: dir + plane * x, x in (-1, 1) over 64 columns.
 * Components scale to |dir| = 88.3 (rcp[] in tables.h assumes that), so
 * the corners (|dir + plane|) stay inside int8. */
static void setup_rays(void)
{
    int16_t c = (int16_t)cos8[cam_a] * 178;
    int16_t s = (int16_t)sin8[cam_a] * 178;

    r_stx = (int16_t)(-s >> 5);          /* plane / 32 */
    r_sty = (int16_t)(c >> 5);
    r_rdx = (int16_t)(c + s + (r_stx >> 1) + 128);   /* dir - plane, +0.5 step, rounded */
    r_rdy = (int16_t)(s - c + (r_sty >> 1) + 128);
}

static void render_view(void)
{
    setup_rays();
    render_asm();                       /* also marks seen[] for every ray's hit */
    PROF(1);
    PROF(2);
}

/* Camera walker for the title and the floor intro: turn to att_d * 64 in
 * steps of 4, then glide ATT_STEP along it; at each cell centre take the
 * first open way of right, ahead, left, back (right-hand wall follower).
 * Expects the camera on a cell centre facing a multiple of 64 at start. */
#define ATT_STEP 8                      /* px per tick (player walks ~16) */
static const int8_t att_off[4] = {1, 32, -1, -32};    /* E S W N in imap */
static uint8_t att_d, att_pick;

static void att_start(void)
{
    att_d = (uint8_t)(cam_a >> 6);
    att_pick = 0u;
}

static void att_step(void)
{
    uint8_t want = (uint8_t)(att_d << 6);
    uint16_t i;

    if (cam_a != want) {
        if ((int8_t)(want - cam_a) > 0) cam_a += 4u;
        else                            cam_a -= 4u;
        return;
    }
    if ((uint8_t)cam_x == 128u && (uint8_t)cam_y == 128u && !att_pick) {
        i = (uint16_t)(((cam_y >> 8) << 5) + (cam_x >> 8));
        att_d = (uint8_t)((att_d + 1u) & 3u);                 /* right first */
        while (imap[i + att_off[att_d]]) att_d = (uint8_t)((att_d + 3u) & 3u);
        att_pick = 1u;
        return;
    }
    att_pick = 0u;
    if (att_d == 0u)      cam_x += ATT_STEP;
    else if (att_d == 1u) cam_y += ATT_STEP;
    else if (att_d == 2u) cam_x -= ATT_STEP;
    else                  cam_y -= ATT_STEP;
}

/* Logic ticks (60 Hz) since the last call, at most 4 per image. */
static uint8_t att_last;
static uint8_t ticks(void)
{
    uint8_t n = (uint8_t)(vbl_count - att_last);

    att_last = (uint8_t)(att_last + n);
    return (n > 4u) ? 4u : n;
}

/* Render the view into the hidden bank; the caller sets sprites and flip_req. */
static void show_view(void)
{
    render_view();
    while (flip_req) ;                   /* last image shown: banks settled */
    PROF(3);
    r_dst = (LCDC_REG & LCDCF_BG8000) ? 0x9000u : 0x8000u;   /* hidden bank */
    copy_view();                         /* only rows that can differ */
#ifdef CHECK
    {
        uint8_t e = check_view();
        if (e > chk_err) chk_err = e;   /* worst image so far, shown by the VBL ISR */
    }
#endif
    PROF(4);
}

/* Title over a live view: the camera walks a floor-1 maze until Start or
 * (with a save) A and a slot. Sprites stay hidden: the font sits on their
 * tiles. Returns non-zero for a load (cur_slot). The caller seeds and
 * loads the real floor. */
static uint8_t title_screen(void)
{
    uint8_t i, can_load = save_ok();
    for (i = 0u; i < MAX_SPR; i++) move_sprite(i, 0u, 0u);
    gun_shown = 0xFFu;
    load_level(0u);
    banner_t = 0u;
    DISPLAY_OFF;
    set_bkg_data(FONT_BASE, FONT_N, font_tiles);
    draw_frame();
    banner(1u, "DUNGEON");
    banner(2u, "ESCAPE 4 FLOORS");
    banner(13u, "PRESS START");
    banner(14u, can_load ? "A LOAD SAVE" : "");
    banner(15u, "BY ZAPSKI");
    view_reset();
    DISPLAY_ON;

    att_start();
    ticks();
    i = start_or_load(can_load, 14u, 1u);

    while (flip_req) ;
    DISPLAY_OFF;
    draw_frame();
    sprites_init();                      /* the font overwrote some */
    DISPLAY_ON;
    return i;
}

/* After a load, with the game paused, while the FLOOR banner shows. New
 * floor: the walker walks away from the spawn facing backwards and the
 * walk is played back reversed, so the camera glides in and stops on the
 * spawn pose. Save: one turn on the spot. Either way the player starts
 * exactly where load_level/load_game put them. Enemies, pickups and the
 * clock wait. The recording borrows ai_dist (ai_reset after). */
#define INTRO_T 90u                      /* ticks, = BANNER_T */
static void intro(void)
{
    uint16_t *rx = (uint16_t *)ai_dist, *ry = rx + INTRO_T;
    uint8_t *ra = (uint8_t *)(ry + INTRO_T);
    uint16_t x0 = cam_x, y0 = cam_y;
    uint8_t a0 = cam_a, t, n;

    if (intro_req == INTRO_GLIDE) {
        cam_a += 128u;
        att_start();
        for (t = 0u; t < INTRO_T; t++) {
            att_step();
            rx[t] = cam_x; ry[t] = cam_y; ra[t] = (uint8_t)(cam_a + 128u);
        }
    }
    t = INTRO_T;
    ticks();
    while (t) {
        for (n = ticks(); n && t; n--) {
            t--;
            if (intro_req == INTRO_GLIDE) {
                cam_x = rx[t]; cam_y = ry[t]; cam_a = ra[t];
            } else if (t >= INTRO_T - 64u) {
                cam_a += 4u;                          /* 64 x 4 = 256: a full turn */
            }
        }
        if (!t) { cam_x = x0; cam_y = y0; cam_a = a0; }
        show_view();
        sprite_update();
        gun_draw(0u, 0u, 0u);
        flip_req = 1u;
        images++;
    }
    ai_reset();
    intro_req = 0u;
}

void main(void)
{
    uint8_t last_vbl, tick = 0u, pad_prev = 0u, fire_cd = 0u, i;

    init_screen();
    sfx_init();
    init_hud();
    init_enemies();
    add_VBL(vbl_handler);
    set_interrupts(VBL_IFLAG);
    DISPLAY_ON;
    i = title_screen();
#ifdef PROFILE
    initrand(0x1234u);
#else
    initrand((uint16_t)(((uint16_t)DIV_REG << 8) | vbl_count));   /* Start timing = seed */
#endif
    if (i) load_game();
    else   load_level(0u);
#ifdef PROFILE
    intro_req = 0u;                      /* profile the start view itself */
#endif
    view_reset();
    last_vbl = vbl_count;

    while (1) {
        if (intro_req) {
            intro();
            last_vbl = vbl_count;        /* no catch-up for the intro */
        }
        /* game logic at 60 Hz regardless of the render rate */
        uint8_t n = (uint8_t)(vbl_count - last_vbl);
        last_vbl = (uint8_t)(last_vbl + n);
        if (n > 4u) n = 4u;
        while (n && !intro_req) {       /* a new floor stops input: the intro needs the spawn pose */
            handle_input();
            post_move();
            {
                uint8_t pad = joypad();
                if ((pad & J_A) && !(pad_prev & J_A) && fire_cd == 0u) {
                    shoot();
                    fire_cd = 15u;
                }
                if ((pad & J_SELECT) && !(pad_prev & J_SELECT)) {
                    show_map();
                    pad = joypad();
                    last_vbl = vbl_count;        /* no catch-up for the pause */
                    n = 1u;
                }
                pad_prev = pad;
                if (fire_cd) fire_cd--;
                /* gun slides down 1 px a tick while an item is behind it, back up 2 */
                if (item_ahead) {
                    if (gun_low < 16u) gun_low++;
                } else if (gun_low > 2u) {
                    gun_low -= 2u;
                } else {
                    gun_low = 0u;
                }
            }
            tick++;
            if (++sec_frames == 60u) { sec_frames = 0u; secs++; }
            if ((tick & 3u) == 0u) enemy_tick();
            n--;
        }
        PROF(0);

        show_view();
        sprite_update();                 /* shadow OAM; pushed by the next VBL */
        PROF(5);
        if (gun_low != gun_low_shown) {
            gun_shift(gun_low);
            gun_low_shown = gun_low;
        }
        /* recoil 5 frames after a shot, once the gun is back up */
        gun_draw(0u, (uint8_t)(fire_cd > 9u && gun_low == 0u), gun_bob);
        flip_req = 1u;                   /* VBL ISR flips LCDC.4 with the OAM push */
        images++;                        /* FPS: counted and shown by the VBL ISR */
        PROF(6);
#ifdef PROFILE
        if (prof_imgs < PROF_N) prof_imgs++;
#endif
    }
}
