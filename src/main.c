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
#include "enemies.h"
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

/* Scratch read/written by render.s — keep names in sync with the asm. */
uint8_t r_dx, r_dy, r_side, r_wall;
uint8_t r_ddx[2], r_ddy[2], r_dist[2], r_lf[4];
uint8_t r_hit[2], r_k;
int16_t r_rdx, r_rdy, r_stx, r_sty;   /* ray dir accumulators, 8.8 */
uint8_t r_top[64], r_bot[64], r_clr[64], r_zb[64];
uint8_t r_wt[64];            /* wall type 1..3 | side << 7 per column */
int16_t r_s2x, r_s2y, r_ox, r_oy;   /* 2*step, odd-column start */
uint8_t *r_hits[64];         /* map cell each ray hit (automap) */
uint8_t r_tc, r_cb[2], r_mxt, r_mnb, r_lo, r_hi, r_j, r_m, r_wl, r_wh;
uint8_t r_y0, r_y1;
uint16_t r_dst;
uint8_t vbuf[2048];          /* 128x64 bitmap in column-major tile order */

uint8_t imap[1024];          /* char map -> collision/ray map */
uint8_t seen[1024];          /* automap: cells walked next to or seen by a ray */
static uint16_t seen_at = 0xFFFFu;
uint16_t cam_x, cam_y;       /* 8.8, 256 = one cell */
uint8_t  cam_a;              /* 0..255, 256 = full turn, 0 = +x */

/* HUD/game state (early: move()/load_level/hud all use it). */
static uint8_t gun_bob = 0u;
static uint8_t hp = HUD_MAX_HP, keys = 0u, floor_no = 1u;
static uint8_t kills = 0u, sec_frames = 0u;   /* run stats for the win screen */
static uint16_t secs = 0u;
static void win_screen(void);
static void game_over(void);
static uint8_t hud_last_floor = 0xFFu;

#define WM(r,c) (*(uint8_t *)(0x9C00u + (r) * 32u + (c)))

/* ---- enemies ---- */
#define NENEMY 8u
static uint16_t ex[NENEMY], ey[NENEMY];
static uint8_t ealive[NENEMY];
static uint8_t dmg_cd;
static uint8_t level_no, item_n, stairs_x, stairs_y;
static uint8_t item_x[8], item_y[8], item_taken[8], item_kind[8];   /* kind: 0 key, 1 health */
static uint8_t ehp[NENEMY], ehit[NENEMY];   /* enemy hits left, hit-flash ticks */


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
        } else if (ch == 'K' || ch == 'H') {
            if (item_n < 8u) {
                item_x[item_n] = (uint8_t)(i & 31u);
                item_y[item_n] = (uint8_t)(i >> 5);
                item_taken[item_n] = 0u;
                item_kind[item_n] = (ch == 'H') ? 1u : 0u;
                item_n++;
            }
        } else if (ch == 'S') {
            stairs_x = (uint8_t)(i & 31u);
            stairs_y = (uint8_t)(i >> 5);
        } else if (ch >= '1' && ch <= '8') {
            if (ne < NENEMY) {
                ex[ne] = (uint16_t)(((uint16_t)(i & 31u) << 8) + 128u);
                ey[ne] = (uint16_t)(((uint16_t)(i >> 5) << 8) + 128u);
                ealive[ne] = 1u;
                ehp[ne] = (n >= 2u) ? 2u : 1u;   /* floors 3-4: two hits */
                ehit[ne] = 0u;
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

/* Key pickup and stairs, checked after movement. */
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
    }

    for (i = 0u; i < item_n; i++) {
        if (!item_taken[i]) {
            if (item_x[i] == cx) {
                if (item_y[i] == cy) {
                    if (item_kind[i] == 0u) {
                        item_taken[i] = 1u;
                        keys++;
                        sfx_play(SFX_KEY);
                    } else if (hp < HUD_MAX_HP) {   /* health: left lying if full */
                        item_taken[i] = 1u;
                        hp++;
                        sfx_play(SFX_KEY);
                    }
                }
            }
        }
    }
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

static void handle_input(void)
{
    uint8_t k = joypad();
    int16_t sp = 16;

    if (k & (J_LEFT | J_RIGHT)) {
        if (k & J_B) {                            /* strafe */
            int16_t sx = (int16_t)(((int16_t)cos8[(uint8_t)(cam_a + 64u)] * sp) >> 7);
            int16_t sy = (int16_t)(((int16_t)sin8[(uint8_t)(cam_a + 64u)] * sp) >> 7);
            if (k & J_LEFT)  move((int16_t)-sx, (int16_t)-sy);
            else             move(sx, sy);
        } else {                                  /* turn */
            if (k & J_LEFT)  cam_a -= 2u;
            else             cam_a += 2u;
        }
    }
    if (k & J_UP) {
        int16_t mx = (int16_t)(((int16_t)cos8[cam_a] * sp) >> 7);
        int16_t my = (int16_t)(((int16_t)sin8[cam_a] * sp) >> 7);
        move(mx, my);
    }
    if (k & J_DOWN) {
        int16_t mx = (int16_t)(((int16_t)cos8[cam_a] * sp) >> 7);
        int16_t my = (int16_t)(((int16_t)sin8[cam_a] * sp) >> 7);
        move((int16_t)-mx, (int16_t)-my);
    }
    /* gun sway while moving (one cycle ~0.7 s), snaps back at rest */
    if (k & (J_UP | J_DOWN)) gun_bob += 6u;
    else if ((k & J_B) && (k & (J_LEFT | J_RIGHT))) gun_bob += 6u;
    else gun_bob = 0u;
}


static uint16_t div16_8(uint16_t n, uint8_t d)
{
    return (uint16_t)(n / d);            /* SDCC's asm divide beats a C loop */
}

static void enemy_tick(void)
{
    ai_update();
    for (uint8_t i = 0; i < NENEMY; i++) {
        int16_t dx, dy, adx, ady;
        if (!ealive[i]) continue;
        dx = (int16_t)(cam_x - ex[i]);
        dy = (int16_t)(cam_y - ey[i]);
        adx = dx < 0 ? (int16_t)-dx : dx;
        ady = dy < 0 ? (int16_t)-dy : dy;
        if (ehit[i]) ehit[i]--;                    /* stunned while flashing */
        else ai_step(i);                           /* follow corridors toward the player */
        if (adx < 200 && ady < 200 && dmg_cd == 0u) {
            if (hp) hp--;
            dmg_cd = 60u;
            flash_pal = 0x1Bu; flash_n = 6u;          /* hurt: invert */
            sfx_play(SFX_HURT);
            if (hp == 0u) {
                game_over();
                load_level(0u);
            }
        }
    }
    if (dmg_cd) dmg_cd--;
}


/* Item sprites (8x16, OBP1 so colour 3 shows white): key, stairs arrow. */
#define SPR_KEY    224u
#define SPR_STAIRS 226u
#define SPR_HEALTH 154u                 /* after the gun tiles */
#define MAX_SPR    40u
const uint8_t item_tiles[4 * 16] = {
    /* 224 key top: empty */
    0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0,
    /* 225 key bottom: ring + bit */
    0,0, 0x38,0x38, 0x44,0x44, 0x38,0x38, 0x10,0x10, 0x1C,0x1C, 0x10,0x10, 0x1C,0x1C,
    /* 226 stairs top: arrow shaft */
    0x18,0x18, 0x18,0x18, 0x18,0x18, 0x18,0x18, 0x18,0x18, 0x18,0x18, 0x18,0x18, 0x18,0x18,
    /* 227 stairs bottom: arrow head */
    0x18,0x18, 0x18,0x18, 0xFF,0xFF, 0x7E,0x7E, 0x3C,0x3C, 0x18,0x18, 0,0, 0,0,
};

const uint8_t health_tiles[2 * 16] = {
    /* 154 top: empty */
    0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0,
    /* 155 bottom: heart */
    0,0, 0x6C,0x6C, 0xFE,0xFE, 0xFE,0xFE, 0x7C,0x7C, 0x38,0x38, 0x10,0x10, 0,0,
};

/* Project a world point (8.8) with the current camera. Returns 1 when it is
 * in range, inside the 90 deg FOV and nearer than the wall at its column
 * (r_zb, 1/8 cells). Sets p_fwd (8.8 depth), p_x (screen px) and p_foot
 * (screen y of the floor under it). */
static int16_t p_fwd;
static uint8_t p_x, p_foot;

static uint8_t project(uint16_t wx, uint16_t wy)
{
    int16_t rx, ry, cs, sn, lat;
    uint16_t n, off, foot;

    rx = (int16_t)wx - (int16_t)cam_x;
    ry = (int16_t)wy - (int16_t)cam_y;
    if (rx > 2560) return 0u;
    if (rx < -2560) return 0u;
    if (ry > 2560) return 0u;
    if (ry < -2560) return 0u;
    /* 1/16-cell precision in int16: |(r>>4)| <= 226 along any axis, *127 fits */
    rx >>= 4;
    ry >>= 4;
    cs = cos8[cam_a];
    sn = sin8[cam_a];
    p_fwd = (int16_t)((rx * cs + ry * sn) >> 3);
    if (p_fwd < 96) return 0u;
    if (p_fwd > 2560) return 0u;
    lat = (int16_t)((ry * cs - rx * sn) >> 3);
    if (lat < 0) n = (uint16_t)-lat;
    else         n = (uint16_t)lat;
    if (n >= (uint16_t)p_fwd) return 0u;            /* outside the FOV */
    off = div16_8((uint16_t)(n << 2), (uint8_t)(p_fwd >> 4));   /* 64*lat/fwd */
    if (off > 60u) return 0u;
    if (lat < 0) p_x = (uint8_t)(80u - off);
    else         p_x = (uint8_t)(80u + off);
    if (r_zb[(uint8_t)(p_x - VIEW_X0) >> 1] < (uint8_t)(p_fwd >> 5)) return 0u;
    /* floor line: horizon + 32 px * (1 cell / fwd) */
    foot = (uint16_t)HORIZON + div16_8(256u, (uint8_t)(p_fwd >> 5));
    if (foot > VIEW_Y0 + 64u) foot = VIEW_Y0 + 64u;
    p_foot = (uint8_t)foot;
    return 1u;
}

static uint8_t enemy_tier(int16_t fwd)
{
    if (fwd < 384) return 2u;
    if (fwd < 768) return 1u;
    return 0u;
}

/* A: hitscan the nearest visible enemy overlapping the screen centre. */
static void shoot(void)
{
    static const uint8_t half[3] = {4u, 8u, 12u};   /* billboard half-widths */
    uint8_t i, best = 0xFFu, d;
    int16_t best_fwd = 0x7FFF;

    flash_pal = 0x90u; flash_n = 2u;                 /* muzzle flash: lighten */
    sfx_play(SFX_SHOOT);
    for (i = 0u; i < NENEMY; i++) {
        if (!ealive[i]) continue;
        if (!project(ex[i], ey[i])) continue;
        d = (p_x > 80u) ? (uint8_t)(p_x - 80u) : (uint8_t)(80u - p_x);
        if (d > half[enemy_tier(p_fwd)]) continue;
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

/* Billboards for the camera the view was just rendered from. */
static void sprite_update(void)
{
    uint8_t si = GUN_SPRITES, drawn = 0u, i, x, top, pal;   /* 0.. = gun */

    for (i = 0u; i < NENEMY; i++) {
        if (drawn >= 3u) break;
        if (!ealive[i]) continue;
        if (!project(ex[i], ey[i])) continue;
        x = p_x;
        pal = 0u;
        if (ehit[i]) pal = S_PALETTE;
        /* OAM coords: screen + (8, 16) */
        switch (enemy_tier(p_fwd)) {
        case 0u:
            top = p_foot;
            set_sprite_tile(si, 206u); set_sprite_prop(si, pal); move_sprite(si, (uint8_t)(x + 4u), top); si++;
            break;
        case 1u:
            top = p_foot;
            set_sprite_tile(si, 208u); set_sprite_prop(si, pal); move_sprite(si, x, top); si++;
            set_sprite_tile(si, 210u); set_sprite_prop(si, pal); move_sprite(si, (uint8_t)(x + 8u), top); si++;
            break;
        default:
            top = (uint8_t)(p_foot - 16u);
            set_sprite_tile(si, 212u); set_sprite_prop(si, pal); move_sprite(si, (uint8_t)(x - 4u), top); si++;
            set_sprite_tile(si, 214u); set_sprite_prop(si, pal); move_sprite(si, (uint8_t)(x + 4u), top); si++;
            set_sprite_tile(si, 216u); set_sprite_prop(si, pal); move_sprite(si, (uint8_t)(x + 12u), top); si++;
            top = (uint8_t)(top + 16u);
            set_sprite_tile(si, 218u); set_sprite_prop(si, pal); move_sprite(si, (uint8_t)(x - 4u), top); si++;
            set_sprite_tile(si, 220u); set_sprite_prop(si, pal); move_sprite(si, (uint8_t)(x + 4u), top); si++;
            set_sprite_tile(si, 222u); set_sprite_prop(si, pal); move_sprite(si, (uint8_t)(x + 12u), top); si++;
            break;
        }
        drawn++;
    }
    /* keys and stairs: one 8x16 each, standing on the floor */
    for (i = 0u; i < item_n; i++) {
        if (item_taken[i]) continue;
        if (si >= MAX_SPR) break;
        if (!project((uint16_t)((item_x[i] << 8) + 128u), (uint16_t)((item_y[i] << 8) + 128u))) continue;
        if (item_kind[i]) set_sprite_tile(si, SPR_HEALTH);
        else              set_sprite_tile(si, SPR_KEY);
        set_sprite_prop(si, S_PALETTE);
        move_sprite(si, (uint8_t)(p_x + 4u), p_foot); si++;
    }
    if (si < MAX_SPR) {
        if (project((uint16_t)((stairs_x << 8) + 128u), (uint16_t)((stairs_y << 8) + 128u))) {
            set_sprite_tile(si, SPR_STAIRS); set_sprite_prop(si, S_PALETTE);
            move_sprite(si, (uint8_t)(p_x + 4u), p_foot); si++;
        }
    }
    while (si < MAX_SPR) { move_sprite(si, 0u, 0u); si++; }
}

/* ---- automap (Select): explored cells, 2x2 px each, 64x64 in the middle
 * of the viewport; the player is an 8-direction arrow sprite. ---- */
#define SPR_ARROW 228u                   /* 8 dirs x (arrow, blank) */
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
    set_sprite_data(206u, 18u, enemy_tiles);
    set_sprite_data(SPR_KEY, 4u, item_tiles);
    set_sprite_data(SPR_HEALTH, 2u, health_tiles);
    set_sprite_data(SPR_ARROW, 16u, arrow_tiles);
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

/* Paused overhead map until Select (or A/B/Start) is pressed again. */
static void show_map(void)
{
    uint8_t si;

    draw_map();
    while (flip_req) ;                   /* hidden bank settled */
    r_dst = (LCDC_REG & LCDCF_BG8000) ? 0x9000u : 0x8000u;
    copy_vbuf();
    /* arrow centre is pixel (3,3) of its tile; OAM = screen + (8, 16) */
    set_sprite_tile(0u, (uint8_t)(SPR_ARROW + ((((uint8_t)(cam_a + 16u)) >> 5) << 1)));
    set_sprite_prop(0u, S_PALETTE);      /* white */
    move_sprite(0u, (uint8_t)(VIEW_X0 + MAP_X0 + (cam_x >> 7) - 3u + 8u),
                    (uint8_t)(VIEW_Y0 + (cam_y >> 7) - 3u + 16u));
    for (si = 1u; si < MAX_SPR; si++) move_sprite(si, 0u, 0u);
    flip_req = 1u;
    while (flip_req) ;

    while (joypad() & J_SELECT) wait_vbl_done();
    while (!(joypad() & (J_SELECT | J_START | J_A | J_B))) wait_vbl_done();
    while (joypad() & (J_SELECT | J_START | J_A | J_B)) wait_vbl_done();
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
    static uint8_t last_hp = 0xFFu, last_keys = 0xFFu;
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
    if (floor_no != hud_last_floor) {
        WM(0, 8) = HUD_DIGIT(floor_no);
        hud_last_floor = floor_no;
    }
}


/* Extra letters for the title/end screens, HUD style (3x5, black on white).
 * O reuses digit 0; E S N W K F come from the HUD set. */
#define FONT_BASE 184u
const uint8_t font_tiles[13 * 16] = {
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
};

static uint8_t glyph(char ch)
{
    static const char extra[] = "YUCAPDTILRGMV";
    static const char hud[] = "NESWKF";
    uint8_t i;

    if (ch >= '0' && ch <= '9') return HUD_DIGIT(ch - '0');
    if (ch == 'O') return HUD_DIGIT(0);
    for (i = 0u; i < 13u; i++) if (extra[i] == ch) return (uint8_t)(FONT_BASE + i);
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

/* Full-screen text (3 lines + PRESS START) until Start is pressed, then
 * the normal frame comes back. */
static void text_screen(const char *l1, const char *l2, const char *l3)
{
    uint8_t i;

    for (i = 0u; i < MAX_SPR; i++) move_sprite(i, 0u, 0u);
    DISPLAY_OFF;
    for (i = 0u; i < 18u; i++) banner(i, "");
    banner(4u, l1);
    banner(7u, l2);
    banner(9u, l3);
    banner(12u, "PRESS START");
    DISPLAY_ON;

    while (joypad() & J_START) wait_vbl_done();
    while (!(joypad() & J_START)) wait_vbl_done();
    while (joypad() & J_START) wait_vbl_done();

    DISPLAY_OFF;
    draw_frame();
    DISPLAY_ON;
}

static void new_run(void)
{
    hp = HUD_MAX_HP;
    kills = 0u;
    secs = 0u;
}

/* Cleared floor 4: stats, then a fresh run (caller loads floor 1). */
static void win_screen(void)
{
    char a[12], b[12];

    label_num(a, "KILLS", kills);
    label_num(b, "SECS", secs);
    text_screen("YOU ESCAPED", a, b);
    new_run();
}

/* Out of hearts: stats, then a fresh run (caller loads floor 1). */
static void game_over(void)
{
    char a[12], b[12];

    label_num(a, "FLOOR", floor_no);
    label_num(b, "KILLS", kills);
    text_screen("GAME OVER", a, b);
    new_run();
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
    set_bkg_data(FONT_BASE, 13u, font_tiles);
    frame_init();
    draw_frame();

    for (uint8_t c = 0; c < 20u; c++) row[c] = T_WHITE;
    set_win_tiles(0u, 0u, 20u, 1u, row);
    row[10] = glyph('E');              /* build marker */
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
    uint8_t k;

    setup_rays();
    render_asm();
    for (k = 0u; k < 64u; k++) seen[(uint16_t)(r_hits[k] - imap)] = 1u;
}

void main(void)
{
    uint8_t last_vbl, tick = 0u, pad_prev = 0u, fire_cd = 0u;

    init_screen();
    sfx_init();
    init_hud();
    init_enemies();
    add_VBL(vbl_handler);
    set_interrupts(VBL_IFLAG);
    DISPLAY_ON;
    text_screen("DUNGEON", "ESCAPE 4 FLOORS", "");
    initrand((uint16_t)(((uint16_t)DIV_REG << 8) | vbl_count));   /* Start timing = seed */
    load_level(0u);
    last_vbl = vbl_count;

    while (1) {
        /* game logic at 60 Hz regardless of the render rate */
        uint8_t n = (uint8_t)(vbl_count - last_vbl);
        last_vbl = (uint8_t)(last_vbl + n);
        if (n > 4u) n = 4u;
        while (n) {
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
            }
            tick++;
            if (++sec_frames == 60u) { sec_frames = 0u; secs++; }
            if ((tick & 3u) == 0u) enemy_tick();
            n--;
        }

        render_view();
        while (flip_req) ;               /* last image shown: banks settled */
        r_dst = (LCDC_REG & LCDCF_BG8000) ? 0x9000u : 0x8000u;   /* hidden bank */
        copy_vbuf();
        sprite_update();                 /* shadow OAM; pushed by the next VBL */
        gun_draw(0u, (uint8_t)(fire_cd > 9u), gun_bob);   /* recoil 5 frames after a shot */
        flip_req = 1u;                   /* VBL ISR flips LCDC.4 with the OAM push */
        images++;                        /* FPS: counted and shown by the VBL ISR */
    }
}
