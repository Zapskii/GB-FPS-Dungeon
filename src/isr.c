/* VBlank handler: frame counter, OAM push, HUD heartbeat.
 *
 * The 3D view is no longer streamed from here: the main loop renders a
 * bitmap into WRAM, copy_vbuf() (render.s) moves it into the hidden VRAM
 * tile bank with STAT polling, and the page flip is a single LCDC.4
 * toggle right after wait_vbl_done().
 */
#include <gbdk/platform.h>
#include <gb/gb.h>
#include <stdint.h>

volatile uint8_t vbl_count = 0u;
volatile uint8_t flash_n = 0u, flash_pal = 0xE4u;   /* BGP override frames */

#define WM(r,c) (*(uint8_t *)(0x9C00u + (r) * 32u + (c)))

void vbl_handler(void)
{
    vbl_count++;
    refresh_OAM();
    if (flash_n) { BGP_REG = flash_pal; flash_n--; }
    else         BGP_REG = 0xE4u;
    if ((vbl_count & 15u) == 0u)
        WM(1, 0) = (vbl_count & 16u) ? 128u : 129u;   /* black / white */
}
