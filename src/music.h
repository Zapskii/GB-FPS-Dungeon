/* Background music on the two channels sfx.h leaves alone: ch2 (square)
 * plucks the melody, ch3 (wave, triangle loaded by sfx_init) plays bass.
 * Tracker layout: 16-step patterns (2 bars of 8 steps), an order list of
 * {lead, bass} pattern pairs, 8 frames a step: 30 entries ~ 64 s, A minor.
 * Ticked from the VBL ISR so the tempo never follows the render rate.
 * Hardware envelope (ch2) and length (ch3) end each note; _ = no trigger.
 * Only included by isr.c.
 */
#ifndef MUSIC_H
#define MUSIC_H

#include <gb/gb.h>
#include <stdint.h>

#define MUS_STEP_FRAMES 8u
#define MUS_ROWS        16u

enum {
    _ = 0,
    C2, Cs2, D2, Ds2, E2, F2, Fs2, G2, Gs2, A2, As2, B2,
    C3, Cs3, D3, Ds3, E3, F3, Fs3, G3, Gs3, A3, As3, B3,
    C4, Cs4, D4, Ds4, E4, F4, Fs4, G4, Gs4, A4, As4, B4,
    C5, Cs5, D5, Ds5, E5, F5, Fs5, G5, Gs5, A5, As5, B5,
    C6
};

/* ch3 period (2048 - 65536/f) for C2..C6. ch2 runs an octave higher on
 * the same value, so a lead note N reads mus_period[N - 13]. */
static const uint16_t mus_period[49] = {
    1046u, 1102u, 1155u, 1205u, 1253u, 1297u, 1339u, 1379u,
    1417u, 1452u, 1486u, 1517u, 1547u, 1575u, 1602u, 1627u,
    1650u, 1673u, 1694u, 1714u, 1732u, 1750u, 1767u, 1783u,
    1798u, 1812u, 1825u, 1837u, 1849u, 1860u, 1871u, 1881u,
    1890u, 1899u, 1907u, 1915u, 1923u, 1930u, 1936u, 1943u,
    1949u, 1954u, 1959u, 1964u, 1969u, 1974u, 1978u, 1982u,
    1985u
};

/* Lead patterns: C3..C6 only. */
static const uint8_t mus_lead[17][MUS_ROWS] = {
    { _,_,_,_,_,_,_,_,     _,_,_,_,_,_,_,_ },                       /*  0 rest */
    { A4,_,_,C5,_,_,E5,_,  D5,_,_,C5,_,_,B4,_ },                    /*  1 Am   */
    { A4,_,_,C5,_,_,F5,_,  E5,_,_,C5,_,_,A4,_ },                    /*  2 F    */
    { G4,_,_,B4,_,_,D5,_,  C5,_,_,B4,_,_,G4,_ },                    /*  3 G    */
    { Gs4,_,_,B4,_,_,E5,_, D5,_,C5,_,B4,_,Gs4,_ },                  /*  4 E    */
    { E5,_,_,_,A5,_,G5,_,  E5,_,_,D5,_,C5,_,_ },                    /*  5 Am   */
    { F5,_,_,_,A5,_,G5,_,  F5,_,_,E5,_,C5,_,_ },                    /*  6 F    */
    { D5,_,_,_,G5,_,F5,_,  D5,_,_,B4,_,G4,_,_ },                    /*  7 G    */
    { B4,_,_,_,E5,_,D5,_,  B4,_,Gs4,_,E4,_,_,_ },                   /*  8 E    */
    { D5,_,_,F5,_,_,A5,_,  G5,_,F5,_,E5,_,D5,_ },                   /*  9 Dm   */
    { C5,_,_,E5,_,_,A5,_,  G5,_,E5,_,C5,_,A4,_ },                   /* 10 Am   */
    { A4,_,C5,_,F5,_,A5,_, G5,_,D5,_,B4,_,G4,_ },                   /* 11 F|G  */
    { Gs4,_,_,B4,_,_,D5,_, E5,_,_,_,_,_,_,_ },                      /* 12 E    */
    { C5,_,_,_,F5,_,_,_,   A5,_,_,_,G5,_,F5,_ },                    /* 13 F    */
    { E5,_,_,_,G5,_,_,_,   C6,_,_,_,G5,_,E5,_ },                    /* 14 C    */
    { F5,_,_,_,D5,_,_,_,   A5,_,_,_,F5,_,D5,_ },                    /* 15 Dm   */
    { B4,_,_,_,E5,_,_,_,   Gs5,_,_,_,E5,_,B4,_ }                    /* 16 E    */
};

/* Bass patterns: root, root, octave per bar. */
#define BASS(r, o, r2, o2) { r,_,_,r,_,_,o,_, r2,_,_,r2,_,_,o2,_ }
static const uint8_t mus_bass[7][MUS_ROWS] = {
    BASS(A2, A3, A2, A3),   /* 0 Am  */
    BASS(F2, F3, F2, F3),   /* 1 F   */
    BASS(G2, G3, G2, G3),   /* 2 G   */
    BASS(E2, E3, E2, E3),   /* 3 E   */
    BASS(D2, D3, D2, D3),   /* 4 Dm  */
    BASS(C2, C3, C2, C3),   /* 5 C   */
    BASS(F2, F3, G2, G3)    /* 6 F|G */
};

/* {lead, bass}: intro, A, A', B, C, A, B, A' */
static const uint8_t mus_order[][2] = {
    {0,0}, {0,3},
    {1,0}, {2,1}, {3,2}, {4,3},
    {5,0}, {6,1}, {7,2}, {8,3},
    {9,4}, {10,0}, {11,6}, {12,3},
    {13,1}, {14,5}, {15,4}, {16,3},
    {1,0}, {2,1}, {3,2}, {4,3},
    {9,4}, {10,0}, {11,6}, {12,3},
    {5,0}, {6,1}, {7,2}, {8,3}
};
#define MUS_ORDER_LEN (sizeof mus_order / sizeof mus_order[0])

static uint8_t mus_ord = 0u, mus_row = 0u, mus_t = 0u;

static void music_tick(void)
{
    uint8_t n;
    uint16_t p;

    if (mus_t) { mus_t--; return; }
    mus_t = MUS_STEP_FRAMES - 1u;
    if ((n = mus_lead[mus_order[mus_ord][0]][mus_row])) {
        p = mus_period[n - 13u];
        NR21_REG = 0x80u;                  /* 50% duty */
        NR22_REG = 0x64u;                  /* vol 6, fade ~0.4 s: under the sfx */
        NR23_REG = (uint8_t)p;
        NR24_REG = (uint8_t)(0x80u | (p >> 8));
    }
    if ((n = mus_bass[mus_order[mus_ord][1]][mus_row])) {
        p = mus_period[n - 1u];
        NR31_REG = 230u;                   /* ~100 ms */
        NR32_REG = 0x40u;                  /* 50% wave level */
        NR33_REG = (uint8_t)p;
        NR34_REG = (uint8_t)(0xC0u | (p >> 8));   /* trigger + length */
    }
    if (++mus_row == MUS_ROWS) {
        mus_row = 0u;
        if (++mus_ord == MUS_ORDER_LEN) mus_ord = 0u;
    }
}

#endif
