/* Bench harness, appended to a scratch copy of main.c (main renamed).
 * Python (PyBoy) writes a view into b_*, sets b_go; the ROM renders it,
 * times render_asm and sprite_update with TIMA (16 T ticks), sets b_done. */
volatile uint8_t b_go, b_done, b_lvl;
volatile uint16_t b_seed, b_cx, b_cy;
volatile uint8_t b_ca;
volatile uint32_t b_t_r, b_t_s;
volatile uint16_t b_t_c;
uint8_t b_seg[241], b_vh[64];
volatile uint8_t b_bank;
static volatile uint16_t t_ovf;

static void tim_isr(void) { t_ovf++; }

static void t_start(void)
{
    disable_interrupts();
    TIMA_REG = 0u;
    t_ovf = 0u;
    IF_REG = 0u;
    enable_interrupts();
}

static uint32_t t_read(void)
{
    uint16_t o1, o2;
    uint8_t t;
    do { o1 = t_ovf; t = TIMA_REG; o2 = t_ovf; } while (o1 != o2);
    return ((uint32_t)o1 << 12) + ((uint32_t)t << 4);
}

void main(void)
{
    TMA_REG = 0u;
    TAC_REG = 0x05u;                     /* 262144 Hz: one tick per 16 T */
    add_TIM(tim_isr);
    set_interrupts(TIM_IFLAG);
    enable_interrupts();
    DISPLAY_ON;
    for (;;) {
        while (!b_go) ;
        b_go = 0u;
        if (b_lvl != 0xFFu) {
            initrand(b_seed);
            load_level(b_lvl);
            view_reset();
        }
        if (b_cx != 0xFFFFu) {
            cam_x = b_cx;
            cam_y = b_cy;
            cam_a = b_ca;
        }
        setup_rays();
        t_start();
        render_asm();
        b_t_r = t_read();
        t_start();
        sprite_update();
        b_t_s = t_read();
        b_bank ^= 1u;
        r_dst = b_bank ? 0x9000u : 0x8000u;
        for (uint8_t q = 0u; q < 64u; q++) b_vh[q] = r_vh[q];   /* pre-copy history (copysim.py) */
        TAC_REG = 0x04u;                 /* 1024 T per tick: no ISR during the copy */
        TIMA_REG = 0u;
        copy_view();
        b_t_c = (uint16_t)TIMA_REG << 10;
        TAC_REG = 0x05u;
        b_done = 1u;
    }
}
