/* Fire-and-forget sound effects on the DMG APU.
 *
 * Each effect is one row of register values: ch1 (square + sweep) and/or
 * ch4 (noise). Hardware length/envelope/sweep end the sound by itself, so
 * there is no per-frame update. ch2/ch3 belong to the music (music.h). A row with NRx4 == 0 skips that channel.
 * A new effect on a busy channel simply cuts the old one off.
 */
#ifndef SFX_H
#define SFX_H

#include <gb/gb.h>
#include <stdint.h>

#define SFX_SHOOT  0u
#define SFX_HURT   1u
#define SFX_KEY    2u
#define SFX_DOOR   3u
#define SFX_STAIRS 4u
#define SFX_KILL   5u
#define SFX_HIT    6u   /* enemy hit but not killed */

/* NR10 NR11 NR12 NR13 NR14 | NR41 NR42 NR43 NR44 */
static const uint8_t sfx_tab[7u][9u] = {
    /* SHOOT: 62 ms mid noise burst */
    { 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,   0x30u, 0xF1u, 0x52u, 0xC0u },
    /* HURT: 250 ms low square, sweep down */
    { 0x2Bu, 0x80u, 0xF2u, 0x00u, 0xC5u,   0x00u, 0x00u, 0x00u, 0x00u },
    /* KEY: 125 ms 25% duty, sweep up (ends on overflow) */
    { 0x24u, 0x60u, 0xC1u, 0xA0u, 0xC6u,   0x00u, 0x00u, 0x00u, 0x00u },
    /* DOOR: low rumble, envelope fades out in ~375 ms */
    { 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,   0x00u, 0xC2u, 0x71u, 0x80u },
    /* STAIRS: slow descending tone, envelope fades out in ~375 ms */
    { 0x3Du, 0x80u, 0xC2u, 0x00u, 0x87u,   0x00u, 0x00u, 0x00u, 0x00u },
    /* KILL: high noise crack + falling square, both 125 ms */
    { 0x1Au, 0xA0u, 0xF1u, 0x80u, 0xC7u,   0x20u, 0xF2u, 0x34u, 0xC0u },
    /* HIT: 60 ms high blip */
    { 0x00u, 0x8Cu, 0xA1u, 0x40u, 0xC7u,   0x00u, 0x00u, 0x00u, 0x00u }
};

static void sfx_init(void)
{
    NR52_REG = 0x80u;   /* APU on (must be first) */
    NR50_REG = 0x77u;   /* max master volume */
    NR51_REG = 0xFFu;   /* all channels to both sides */
    {                   /* triangle wave for the music bass (music.h) */
        static const uint8_t tri[16] = {
            0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu, 0xCDu, 0xEFu,
            0xFEu, 0xDCu, 0xBAu, 0x98u, 0x76u, 0x54u, 0x32u, 0x10u
        };
        uint8_t i;
        NR30_REG = 0x00u;                      /* DAC off to write wave RAM */
        for (i = 0u; i < 16u; i++) ((volatile uint8_t *)0xFF30u)[i] = tri[i];
        NR30_REG = 0x80u;
    }
}

static void sfx_play(uint8_t id)
{
    const uint8_t *r;

    if (id > SFX_HIT) return;
    r = sfx_tab[id];
    if (r[4]) {
        NR10_REG = r[0];
        NR11_REG = r[1];
        NR12_REG = r[2];
        NR13_REG = r[3];
        NR14_REG = r[4];   /* trigger */
    }
    if (r[8]) {
        NR41_REG = r[5];
        NR42_REG = r[6];
        NR43_REG = r[7];
        NR44_REG = r[8];   /* trigger */
    }
}

#endif
