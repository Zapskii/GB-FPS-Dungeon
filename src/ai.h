/* Enemy pathfinding: BFS distance field from the player's cell over floor
 * cells, enemies walk downhill along corridors. Include after imap, cam_x/y,
 * ex/ey/ealive, NENEMY and blocked() are declared. */
#ifndef AI_H
#define AI_H

#include <string.h>

#define AI_MAX_DIST 10u     /* BFS depth cap: farther enemies idle */
#define AI_QLEN     128u    /* max cells visited per rebuild (bounds cost) */
#define AI_SPEED    6u
#define AI_FAR      255u

static uint8_t ai_dist[1024];
static uint16_t ai_q[AI_QLEN];
static uint16_t ai_pcell;   /* player cell of the current field */

static void ai_reset(void)
{
    ai_pcell = 0xFFFFu;
}

/* BFS from cell s. Linear queue; when full, further cells stay 255. */
static void ai_build(uint16_t s)
{
    uint8_t head, tail, d, k;
    uint16_t c, n;

    memset(ai_dist, AI_FAR, sizeof(ai_dist));
    ai_dist[s] = 0u;
    ai_q[0] = s;
    head = 0u;
    tail = 1u;
    while (head != tail) {
        c = ai_q[head];
        head++;
        d = ai_dist[c];
        if (d >= AI_MAX_DIST) continue;
        d++;
        for (k = 0u; k < 4u; k++) {
            if (k == 0u) n = c + 1u;
            else if (k == 1u) n = c - 1u;
            else if (k == 2u) n = c + 32u;
            else n = c - 32u;
            if (imap[n] == 0u) {
                if (ai_dist[n] == AI_FAR) {
                    if (tail < AI_QLEN) {
                        ai_dist[n] = d;
                        ai_q[tail] = n;
                        tail++;
                    }
                }
            }
        }
    }
}

static void ai_update(void)
{
    uint16_t c = ((cam_y >> 8) << 5) + (cam_x >> 8);
    if (c != ai_pcell) {
        ai_pcell = c;
        ai_build(c);
    }
}

/* Signed step from p toward t, at most AI_SPEED, never overshooting. */
static int16_t ai_toward(uint16_t p, uint16_t t)
{
    int16_t d = (int16_t)(t - p);
    if (d > (int16_t)AI_SPEED) return (int16_t)AI_SPEED;
    if (d < -(int16_t)AI_SPEED) return -(int16_t)AI_SPEED;
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
