/* VBlank handler: frame counter, bank flip, OAM push, flashes, HUD, music.
 *
 * The 3D view is no longer streamed from here: the main loop renders a
 * bitmap into WRAM, copy_vbuf() (render.s) moves it into the hidden VRAM
 * tile bank with STAT polling, then sets flip_req; this handler toggles
 * LCDC.4 in the same VBlank as the OAM push, so the main loop never idles
 * waiting for VBlank. Keep it well inside VBlank (~4.5k T): copy_vbuf's
 * safety relies on the ISR ending before line 0.
 */
#include <gbdk/platform.h>
#include <gb/gb.h>
#include <stdint.h>
#include "music.h"

volatile uint8_t vbl_count = 0u;
volatile uint8_t flash_n = 0u, flash_pal = 0xE4u;   /* BGP override frames */
volatile uint8_t flip_req = 0u;                       /* main: new image ready */
volatile uint8_t images = 0u;                         /* main: views rendered */
static uint8_t fps_t = 0u;

#define WM(r,c) (*(uint8_t *)(0x9C00u + (r) * 32u + (c)))
#define HUD_DIGIT0 160u                               /* HUD_BASE in main.c */
void hud_update(void);

void vbl_handler(void)
{
    vbl_count++;
    if (flip_req) {
        LCDC_REG ^= LCDCF_BG8000;
        flip_req = 0u;
    }
    refresh_OAM();
    if (flash_n) { BGP_REG = flash_pal; flash_n--; }
    else         BGP_REG = 0xE4u;
    hud_update();
    music_tick();
    if (++fps_t == 60u) {                /* once a second: views shown, HUD row 1 right */
        uint8_t n = images;
        images = 0u;
        fps_t = 0u;
        if (n > 99u) n = 99u;
        WM(1, 17) = (uint8_t)(HUD_DIGIT0 + n / 10u);
        WM(1, 18) = (uint8_t)(HUD_DIGIT0 + n % 10u);
    }
}
