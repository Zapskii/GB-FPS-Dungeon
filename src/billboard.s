; GB-FPS-Dungeon billboards: draw_art in assembly.
; The C version called GBDK's set_sprite_tile/set_sprite_prop/move_sprite
; per tile, each one re-deriving &shadow_OAM[nb]; this writes the four
; OAM bytes per slot directly. Same art format and results: {w, h,
; tile|flip...}, row-major, bottom centre at screen (x, y), stops before
; OAM slot MAX_SPR (40).

        .module billboard
        .area   _CODE

MAXSPR  = 40                    ; MAX_SPR in main.c

; Per-call cursor state. The registers are all spoken for (DE art, HL
; shadow_OAM + si*4, B si, C palette), so those live in HRAM.
        .area   _HRAM
ba_w0:  .ds     1               ; art width
ba_wl:  .ds     1               ; columns left in this row
ba_h:   .ds     1               ; rows left
ba_ox:  .ds     1               ; this tile's OAM x; +8 per column
ba_oy:  .ds     1               ; this row's OAM y; +16 per row
ba_x0:  .ds     1               ; first column's OAM x

        .area   _CODE

; draw_art: one billboard from {w, h, tile|flip...} into shadow_OAM from
; slot si, bottom centre at screen (x, y). sdcccall(1): si in A, art in
; DE, x/y/pal at sp+2/3/4 on entry. Returns the next free slot in A (si
; unchanged when si >= MAX_SPR, w == 0 or h == 0). As in SDCC's own
; epilogue, the callee drops the 3 stack bytes before returning; BC, DE
; and HL are caller-saved.
_draw_art::
        ld      b, a                    ; B = si
        ld      hl, #2
        add     hl, sp                  ; x, y, pal
        ld      a, (hl+)                ; x
        ldh     (ba_x0), a              ; park x until w is known
        ld      a, (hl+)                ; y
        ld      c, a                    ; C = y
        ld      a, (hl)                 ; pal
        ld      l, a                    ; L = pal
        ld      a, (de)                 ; w
        ldh     (ba_w0), a
        ldh     (ba_wl), a
        add     a, a
        add     a, a                    ; (w << 2) & 0xff
        ld      h, a
        ldh     a, (ba_x0)
        sub     a, h
        add     a, #8
        ldh     (ba_x0), a              ; x0 = x - (w << 2) + 8
        inc     de
        ld      a, (de)                 ; h
        ldh     (ba_h), a
        inc     de                      ; art += 2
        swap    a
        and     #0xf0                   ; (h << 4) & 0xff
        ld      h, a
        ld      a, c
        sub     a, h
        add     a, #16
        ldh     (ba_oy), a              ; oy = y - (h << 4) + 16
        ld      c, l                    ; C = palette
        ldh     a, (ba_w0)
        or      a
        jr      z, ba_out               ; w == 0: nothing drawn
        ldh     a, (ba_h)
        or      a
        jr      z, ba_out               ; h == 0: nothing drawn
        ldh     a, (ba_x0)
        ldh     (ba_ox), a
        ld      a, b                    ; HL = shadow_OAM + si * 4
        ld      l, a
        ld      h, #0
        add     hl, hl
        add     hl, hl
        ld      a, l
        add     a, #<(_shadow_OAM)
        ld      l, a
        ld      a, h
        adc     a, #>(_shadow_OAM)
        ld      h, a

; OAM entry per art tile: y, x, tile (bit 0 masked), prop (palette|flip).
ba_tile:
        ld      a, b
        cp      #MAXSPR
        jr      nc, ba_out              ; si >= MAX_SPR: nothing written
        ldh     a, (ba_oy)
        ld      (hl+), a
        ldh     a, (ba_ox)
        ld      (hl+), a
        ld      a, (de)
        and     #0xfe
        ld      (hl+), a
        ld      a, (de)
        inc     de
        and     #1
        rrca
        rrca
        rrca                            ; 0x20 when the tile is x-flipped
        or      c
        ld      (hl+), a
        inc     b                       ; si++
        ldh     a, (ba_ox)
        add     a, #8
        ldh     (ba_ox), a
        ldh     a, (ba_wl)
        dec     a
        ldh     (ba_wl), a
        jr      nz, ba_tile
        ldh     a, (ba_h)
        dec     a
        ldh     (ba_h), a
        jr      z, ba_out               ; last row: no next-row setup
        ldh     a, (ba_oy)
        add     a, #16
        ldh     (ba_oy), a
        ldh     a, (ba_x0)
        ldh     (ba_ox), a
        ldh     a, (ba_w0)
        ldh     (ba_wl), a
        jr      ba_tile

ba_out:
        ld      a, b
        pop     hl                      ; return address
        add     sp, #3                  ; x, y, pal: callee drops them
        jp      (hl)
