; GB-FPS-Dungeon render engine: pixel raycaster.
; 64 rays (one per 2-px column) over a 128x64 bitmap viewport. Ray
; directions come from the camera plane (dir + plane*x, set up by C in
; r_rdx/r_rdy + r_stx/r_sty), so the DDA distance is already the
; perpendicular distance: no fisheye correction needed.
; The bitmap lives in WRAM (_vbuf) laid out exactly like 128 column-major
; tiles: tile column tc is 128 contiguous bytes, pixel row y at tc*128+y*2.
; copy_vbuf streams it to the hidden VRAM tile bank between mode-3 periods.

        .module render
        .area   _CODE

STATR   = 0x41                  ; ldh offset of rSTAT
FOG_NEAR = 3                    ; cells (r_dist hi): wall starts to dither
FOG_FAR  = 6                    ; cells: wall one full shade darker


; ---------------------------------------------------------------
; mul8: HL = B * C  via log/antilog (~80 T; ~2% error, fine for 8px
; bands). Returns 0 if either operand is 0. Clobbers A, HL, DE.
; ---------------------------------------------------------------
mul8:
        ld      a, b
        or      a
        jr      Z, mul_z
        ld      a, c
        or      a
        jr      Z, mul_z
        ld      h, #0
        ld      l, b
        ld      de, #_log8
        add     hl, de
        ld      a, (hl)
        ld      b, a                   ; B = log b (operand B done)
        ld      h, #0
        ld      l, c
        ld      de, #_log8
        add     hl, de
        ld      a, (hl)                ; A = log c
        add     a, b                   ; 9-bit sum: low + carry
        ld      e, a
        ld      a, #0
        adc     a, #0
        ld      d, a                   ; DE = sum (0..511)
        ld      hl, #_antilog
        add     hl, de
        add     hl, de                 ; + 2*sum
        ld      a, (hl+)
        ld      h, (hl)
        ld      l, a
        ret
mul_z:
        ld      hl, #0
        ret

; ---------------------------------------------------------------
; emit_run: B = count, A = tile, HL = data ptr, DE = 3 (stride)
; ---------------------------------------------------------------

; ---------------------------------------------------------------
; sdx = frac * ddx >> 8, split into two fast multiplies
; (frac == 0 -> 0; frac == 256 -> ddx)
; ---------------------------------------------------------------
cr_mulx:
        ld      a, (_r_frac+1)
        or      a
        jr      NZ, cr_mxf
        ld      a, (_r_frac)
        or      a
        jr      Z, cr_mxz
        ld      a, (_r_ddx+1)
        ld      c, a
        ld      a, (_r_frac)
        ld      b, a
        call    mul8
        ld      a, l
        ld      (_r_sdx), a
        ld      a, h
        ld      (_r_sdx+1), a
        ld      a, (_r_ddx)
        ld      c, a
        ld      a, (_r_frac)
        ld      b, a
        call    mul8
        ld      a, h
        ld      b, a
        ld      a, (_r_sdx)
        add     a, b
        ld      (_r_sdx), a
        ld      a, (_r_sdx+1)
        adc     a, #0
        ld      (_r_sdx+1), a
        ret
cr_mxz:
        xor     a
        ld      (_r_sdx), a
        ld      (_r_sdx+1), a
        ret
cr_mxf:
        ld      a, (_r_ddx)
        ld      (_r_sdx), a
        ld      a, (_r_ddx+1)
        ld      (_r_sdx+1), a
        ret

; ---------------------------------------------------------------
; sdy = frac * ddy >> 8
; ---------------------------------------------------------------
cr_muly:
        ld      a, (_r_frac+1)
        or      a
        jr      NZ, cr_myf
        ld      a, (_r_frac)
        or      a
        jr      Z, cr_myz
        ld      a, (_r_ddy+1)
        ld      c, a
        ld      a, (_r_frac)
        ld      b, a
        call    mul8
        ld      a, l
        ld      (_r_sdy), a
        ld      a, h
        ld      (_r_sdy+1), a
        ld      a, (_r_ddy)
        ld      c, a
        ld      a, (_r_frac)
        ld      b, a
        call    mul8
        ld      a, h
        ld      b, a
        ld      a, (_r_sdy)
        add     a, b
        ld      (_r_sdy), a
        ld      a, (_r_sdy+1)
        adc     a, #0
        ld      (_r_sdy+1), a
        ret
cr_myz:
        xor     a
        ld      (_r_sdy), a
        ld      (_r_sdy+1), a
        ret
cr_myf:
        ld      a, (_r_ddy)
        ld      (_r_sdy), a
        ld      a, (_r_ddy+1)
        ld      (_r_sdy+1), a
        ret

; ---------------------------------------------------------------
; cast_ray: A = ray angle index -> _r_dist, _r_wall
; ---------------------------------------------------------------
; ---------------------------------------------------------------
; cast_ray: ray (_r_dx, _r_dy) from the camera -> _r_dist (perp, 8.8),
; _r_wall, _r_side, _r_hit (map cell address).
; ---------------------------------------------------------------
cast_ray:
        ; x: adx, sx
        ld      a, (_r_dx)
        ld      b, a
        bit     7, b
        jr      Z, cr_dxp
        ld      a, b
        cpl
        inc     a
        ld      (_r_adx), a
        ld      a, #1
        ld      (_r_sx), a
        jr      cr_dxn
cr_dxp:
        ld      a, b
        ld      (_r_adx), a
        xor     a
        ld      (_r_sx), a
cr_dxn:
        ; y
        ld      a, (_r_dy)
        ld      b, a
        bit     7, b
        jr      Z, cr_dyp
        ld      a, b
        cpl
        inc     a
        ld      (_r_ady), a
        ld      a, #1
        ld      (_r_sy), a
        jr      cr_dyn
cr_dyp:
        ld      a, b
        ld      (_r_ady), a
        xor     a
        ld      (_r_sy), a
cr_dyn:

        ; ddx = recip8[adx*2]
        ld      a, (_r_adx)
        add     a, a
        ld      e, a
        ld      d, #0
        ld      hl, #_rcp
        add     hl, de
        ld      a, (hl+)
        ld      (_r_ddx), a
        ld      a, (hl)
        ld      (_r_ddx+1), a
        ; ddy
        ld      a, (_r_ady)
        add     a, a
        ld      e, a
        ld      d, #0
        ld      hl, #_rcp
        add     hl, de
        ld      a, (hl+)
        ld      (_r_ddy), a
        ld      a, (hl)
        ld      (_r_ddy+1), a

        ; initial x side distance
        ld      a, (_r_sx)
        or      a
        jr      NZ, cr_sxn
        ld      a, (_cam_x)
        ld      b, a
        xor     a
        sub     b
        ld      (_r_frac), a
        ld      a, #1
        sbc     a, #0
        ld      (_r_frac+1), a
        jr      cr_sxd
cr_sxn:
        ld      a, (_cam_x)
        ld      (_r_frac), a
        xor     a
        ld      (_r_frac+1), a
cr_sxd:
        call    cr_mulx
        ; initial y side distance
        ld      a, (_r_sy)
        or      a
        jr      NZ, cr_syn
        ld      a, (_cam_y)
        ld      b, a
        xor     a
        sub     b
        ld      (_r_frac), a
        ld      a, #1
        sbc     a, #0
        ld      (_r_frac+1), a
        jr      cr_syd
cr_syn:
        ld      a, (_cam_y)
        ld      (_r_frac), a
        xor     a
        ld      (_r_frac+1), a
cr_syd:
        call    cr_muly

        ; HL = imap + mapy*32 + mapx
        ; (done BEFORE loading BC/DE: the address math uses DE as scratch)
        ld      a, (_cam_y+1)
        ld      l, a
        ld      h, #0
        add     hl, hl
        add     hl, hl
        add     hl, hl
        add     hl, hl
        add     hl, hl
        ld      a, (_cam_x+1)
        ld      e, a
        ld      d, #0
        add     hl, de
        ld      de, #_imap
        add     hl, de

        ; side distances into BC (sdx) and DE (sdy)
        ld      a, (_r_sdx)
        ld      c, a
        ld      a, (_r_sdx+1)
        ld      b, a
        ld      a, (_r_sdy)
        ld      e, a
        ld      a, (_r_sdy+1)
        ld      d, a

        xor     a
        ld      (_r_steps), a

dda_top:
        ld      a, (_r_steps)
        inc     a
        ld      (_r_steps), a
        cp      #65
        jp      NC, dda_cap
        ; if (sdx < sdy) step x else step y
        ld      a, b
        cp      d
        jp      C, dda_x
        jr      NZ, dda_y
        ld      a, c
        cp      e
        jp      C, dda_x
dda_y:
        ; sdy(DE) += ddy
        ld      a, (_r_ddy)
        add     a, e
        ld      e, a
        ld      a, (_r_ddy+1)
        adc     a, d
        ld      d, a
        ld      a, #1
        ld      (_r_side), a
        ld      a, (_r_sy)
        or      a
        jr      NZ, dda_ym
        ld      a, l
        add     a, #32
        ld      l, a
        ld      a, h
        adc     a, #0
        ld      h, a
        jr      dda_look
dda_ym:
        ld      a, l
        sub     #32
        ld      l, a
        ld      a, h
        sbc     a, #0
        ld      h, a
        jr      dda_look
dda_x:
        ; sdx(BC) += ddx
        ld      a, (_r_ddx)
        add     a, c
        ld      c, a
        ld      a, (_r_ddx+1)
        adc     a, b
        ld      b, a
        xor     a
        ld      (_r_side), a
        ld      a, (_r_sx)
        or      a
        jr      NZ, dda_xm
        inc     hl
        jr      dda_look
dda_xm:
        dec     hl
dda_look:
        ld      a, (hl)
        or      a
        jr      NZ, dda_hit
        jp      dda_top
dda_cap:
        ld      a, (hl)
        or      a
        jr      NZ, dda_hit
        ld      a, #1
dda_hit:
        ld      (_r_wall), a
        ld      a, l
        ld      (_r_hit), a
        ld      a, h
        ld      (_r_hit+1), a
        ld      a, (_r_side)
        or      a
        jr      NZ, dda_dy
        ; dist = sdx(BC) - ddx
        ld      a, c
        ld      (_r_dist), a
        ld      a, b
        ld      (_r_dist+1), a
        ld      a, (_r_dist)
        ld      b, a
        ld      a, (_r_ddx)
        ld      c, a
        ld      a, b
        sub     c
        ld      (_r_dist), a
        ld      a, (_r_dist+1)
        ld      b, a
        ld      a, (_r_ddx+1)
        ld      c, a
        ld      a, b
        sbc     a, c
        ld      (_r_dist+1), a
        ret
dda_dy:
        ; dist = sdy(DE) - ddy
        ld      a, e
        ld      (_r_dist), a
        ld      a, d
        ld      (_r_dist+1), a
        ld      a, (_r_dist)
        ld      b, a
        ld      a, (_r_ddy)
        ld      c, a
        ld      a, b
        sub     c
        ld      (_r_dist), a
        ld      a, (_r_dist+1)
        ld      b, a
        ld      a, (_r_ddy+1)
        ld      c, a
        ld      a, b
        sbc     a, c
        ld      (_r_dist+1), a
        ret

; ---------------------------------------------------------------
; render_asm: cast 64 rays, then fill the 16 tile columns of _vbuf.
; Per ray k: r_top/r_bot (pixel rows), r_clr (colour 0..3), r_zb
; (perp distance in 1/8 cells, for sprite occlusion).
; Colours: x-facing wall 0, y-facing wall 1, door 2; the first ray on a
; new map cell or face draws its edge one shade darker (2, door 3).
; ---------------------------------------------------------------
_render_asm::
        xor     a
        ld      (_r_k), a
ray_loop:
        ld      a, (_r_rdx+1)
        ld      (_r_dx), a
        ld      a, (_r_rdy+1)
        ld      (_r_dy), a
        call    cast_ray
        ; step the ray direction along the camera plane
        ld      a, (_r_stx)
        ld      b, a
        ld      a, (_r_rdx)
        add     a, b
        ld      (_r_rdx), a
        ld      a, (_r_stx+1)
        ld      b, a
        ld      a, (_r_rdx+1)
        adc     a, b
        ld      (_r_rdx+1), a
        ld      a, (_r_sty)
        ld      b, a
        ld      a, (_r_rdy)
        add     a, b
        ld      (_r_rdy), a
        ld      a, (_r_sty+1)
        ld      b, a
        ld      a, (_r_rdy+1)
        adc     a, b
        ld      (_r_rdy+1), a

        ; H = ht[min(perp >> 3, 1023)]
        ld      a, (_r_dist+1)
        cp      #0x20
        jr      C, rh_near
        ld      hl, #(_ht + 1023)
        jr      rh_have
rh_near:
        ld      d, a
        ld      a, (_r_dist)
        ld      e, a
        srl     d
        rr      e
        srl     d
        rr      e
        srl     d
        rr      e
        ld      hl, #_ht
        add     hl, de
rh_have:
        ld      c, (hl)                ; C = H (2..64)
        ld      a, #64
        sub     c
        srl     a
        ld      b, a                   ; B = top
        add     a, c
        ld      c, a                   ; C = bot
        ld      a, (_r_k)
        ld      e, a
        ld      d, #0                  ; DE = k (kept to the end of the ray)
        ld      hl, #_r_top
        add     hl, de
        ld      (hl), b
        ld      hl, #_r_bot
        add     hl, de
        ld      (hl), c

        ; zb = min(255, perp >> 5)
        ld      a, (_r_dist+1)
        cp      #0x20
        jr      C, zb_s
        ld      a, #0xFF
        jr      zb_w
zb_s:
        ld      b, a
        ld      a, (_r_dist)
        swap    a
        srl     a
        and     #0x07                  ; lo >> 5
        ld      c, a
        ld      a, b
        add     a, a
        add     a, a
        add     a, a                   ; hi << 3
        or      c
zb_w:
        ld      hl, #_r_zb
        add     hl, de
        ld      (hl), a

        ; colour
        ld      a, (_r_wall)
        cp      #3
        jr      NZ, cl_wall
        ld      b, #2
        jr      cl_edge
cl_wall:
        ld      a, (_r_side)
        ld      b, a
cl_edge:
        ld      a, (_r_k)
        or      a
        jr      Z, cl_keep
        ld      hl, #_r_prevp
        ld      a, (_r_hit)
        cp      (hl)
        jr      NZ, cl_dark
        inc     hl
        ld      a, (_r_hit+1)
        cp      (hl)
        jr      NZ, cl_dark
        ld      a, (_r_prevs)
        ld      c, a
        ld      a, (_r_side)
        cp      c
        jr      Z, cl_keep
cl_dark:
        ld      a, b
        cp      #2
        ld      b, #2                  ; wall edge
        jr      C, cl_keep
        ld      b, #3                  ; door edge
cl_keep:
        ld      a, (_r_hit)
        ld      (_r_prevp), a
        ld      a, (_r_hit+1)
        ld      (_r_prevp+1), a
        ld      a, (_r_side)
        ld      (_r_prevs), a
        ld      hl, #_r_hits           ; automap: remember the cell this ray saw
        add     hl, de
        add     hl, de
        ld      a, (_r_hit)
        ld      (hl+), a
        ld      a, (_r_hit+1)
        ld      (hl), a
        ; fog (walls, colour < 2): right pixel one shade darker from
        ; FOG_NEAR cells, both pixels from FOG_FAR (1-px vertical dither).
        ; r_clr = left | right << 2. Doors/edges (2, 3) are left alone so
        ; nothing fades into the black horizon band.
        ld      c, b
        ld      a, b
        cp      #2
        jr      NC, fg_pk
        ld      a, (_r_dist+1)
        cp      #FOG_NEAR
        jr      C, fg_pk
        inc     c
        cp      #FOG_FAR
        jr      C, fg_pk
        inc     b
fg_pk:
        ld      a, c
        add     a, a
        add     a, a
        or      b
        ld      b, a
        ld      hl, #_r_clr
        add     hl, de
        ld      (hl), b

        ld      a, (_r_k)
        inc     a
        ld      (_r_k), a
        cp      #64
        jp      NZ, ray_loop

; ---- fill: per tile column (4 rays), rows [0,maxtop) ceiling,
; [maxtop,minbot) all-wall, [minbot,64) floor, then patch each ray's
; ragged ends with read-modify-write. Every top <= 31 < 33 <= every
; bot (ht >= 2), so the all-wall core is never empty.
        xor     a
        ld      (_r_tc), a
fill_col:
        ; column base = vbuf + tc*128
        ld      a, (_r_tc)
        srl     a
        ld      h, a
        ld      a, #0
        rra
        ld      l, a
        ld      de, #_vbuf
        add     hl, de
        ld      a, l
        ld      (_r_cb), a
        ld      a, h
        ld      (_r_cb+1), a

        ; DE = 4*tc
        ld      a, (_r_tc)
        add     a, a
        add     a, a
        ld      e, a
        ld      d, #0
        ; maxtop
        ld      hl, #_r_top
        add     hl, de
        ld      a, (hl+)
        ld      b, (hl)
        cp      b
        jr      NC, mt1
        ld      a, b
mt1:
        inc     hl
        ld      b, (hl)
        cp      b
        jr      NC, mt2
        ld      a, b
mt2:
        inc     hl
        ld      b, (hl)
        cp      b
        jr      NC, mt3
        ld      a, b
mt3:
        ld      (_r_mxt), a
        ; minbot
        ld      hl, #_r_bot
        add     hl, de
        ld      a, (hl+)
        ld      b, (hl)
        cp      b
        jr      C, mb1
        ld      a, b
mb1:
        inc     hl
        ld      b, (hl)
        cp      b
        jr      C, mb2
        ld      a, b
mb2:
        inc     hl
        ld      b, (hl)
        cp      b
        jr      C, mb3
        ld      a, b
mb3:
        ld      (_r_mnb), a
        ; pack the 4 colours into one lo/hi byte pair (ray 0 = bits 7-6)
        ld      hl, #_r_clr
        add     hl, de
        ld      b, #4
pk_loop:
        ld      a, (hl+)
        rrca                           ; left lo
        rl      c
        rrca                           ; left hi
        rl      e
        rrca                           ; right lo
        rl      c
        rrca                           ; right hi
        rl      e
        dec     b
        jr      NZ, pk_loop
        ld      a, c
        ld      (_r_lo), a
        ld      a, e
        ld      (_r_hi), a

        ; ceiling: rows [0, maxtop) from bg_tab
        ld      a, (_r_cb)
        ld      l, a
        ld      a, (_r_cb+1)
        ld      h, a
        ld      a, (_r_mxt)
        or      a
        jr      Z, f_core
        ld      b, a
        ld      de, #bg_tab
        call    bg_copy
f_core:
        ld      a, (_r_mxt)
        ld      c, a
        ld      a, (_r_mnb)
        sub     c
        ld      b, a
        ld      a, (_r_lo)
        ld      d, a
        ld      a, (_r_hi)
        ld      e, a
f_cl:
        ld      a, d
        ld      (hl+), a
        ld      a, e
        ld      (hl+), a
        dec     b
        jr      NZ, f_cl
        ; floor: rows [minbot, 64) from bg_tab
        ld      a, (_r_mnb)
        ld      c, a
        ld      a, #64
        sub     c
        jr      Z, f_rag
        ld      b, a
        push    hl
        ld      h, #0
        ld      l, c
        add     hl, hl
        ld      de, #bg_tab
        add     hl, de
        ld      d, h
        ld      e, l
        pop     hl
        call    bg_copy

f_rag:
        xor     a
        ld      (_r_j), a
        ld      a, #0xC0
        ld      (_r_m), a
rg_loop:
        ld      a, (_r_tc)
        add     a, a
        add     a, a
        ld      b, a
        ld      a, (_r_j)
        add     a, b
        ld      e, a
        ld      d, #0                  ; DE = ray index
        ld      hl, #_r_clr
        add     hl, de
        ld      b, (hl)            ; bits 0-1 left colour, 2-3 right
        ld      a, (_r_m)
        ld      c, a
        ld      hl, #0             ; H = wl, L = wh
        bit     0, b
        jr      Z, rg1
        ld      a, c
        and     #0xAA
        ld      h, a
rg1:
        bit     1, b
        jr      Z, rg2
        ld      a, c
        and     #0xAA
        ld      l, a
rg2:
        bit     2, b
        jr      Z, rg3
        ld      a, c
        and     #0x55
        or      h
        ld      h, a
rg3:
        bit     3, b
        jr      Z, rg4
        ld      a, c
        and     #0x55
        or      l
        ld      l, a
rg4:
        ld      a, h
        ld      (_r_wl), a
        ld      a, l
        ld      (_r_wh), a
        ld      hl, #_r_top
        add     hl, de
        ld      a, (hl)
        ld      (_r_y0), a
        ld      hl, #_r_bot
        add     hl, de
        ld      a, (hl)
        ld      (_r_y1), a
        ; rows [top, maxtop)
        ld      a, (_r_y0)
        ld      b, a
        ld      a, (_r_mxt)
        sub     b
        jr      Z, rg_bot
        ld      b, a
        ld      a, (_r_y0)
        call    rg_addr
        call    rg_rmw
rg_bot:
        ; rows [minbot, bot)
        ld      a, (_r_mnb)
        ld      c, a
        ld      a, (_r_y1)
        sub     c
        jr      Z, rg_next
        ld      b, a
        ld      a, (_r_mnb)
        call    rg_addr
        call    rg_rmw
rg_next:
        ld      a, (_r_m)
        srl     a
        srl     a
        ld      (_r_m), a
        ld      a, (_r_j)
        inc     a
        ld      (_r_j), a
        cp      #4
        jp      NZ, rg_loop

        ld      a, (_r_tc)
        inc     a
        ld      (_r_tc), a
        cp      #16
        jp      NZ, fill_col
        ret

; B rows from table DE to HL (2 bytes per row).
bg_copy:
        ld      a, (de)
        inc     de
        ld      (hl+), a
        ld      a, (de)
        inc     de
        ld      (hl+), a
        dec     b
        jr      NZ, bg_copy
        ret

; Ceiling/floor pattern per pixel row (lo, hi): row 0 = top
; (nearest ceiling), 32 = horizon, 63 = nearest floor. Ceiling: solid 2,
; 2/3 horizontal lines, sparse lines, black at the horizon. Floor: black,
; 25% dots, 2/3 checker, solid 2, 1/2 checker at the bottom.
bg_tab:
        .db     0x00,0xFF, 0x00,0xFF, 0x00,0xFF, 0x00,0xFF   ; rows 0-3
        .db     0x00,0xFF, 0x00,0xFF, 0x00,0xFF, 0x00,0xFF   ; rows 4-7
        .db     0x00,0xFF, 0x00,0xFF, 0x00,0xFF, 0xFF,0xFF   ; rows 8-11
        .db     0x00,0xFF, 0xFF,0xFF, 0x00,0xFF, 0xFF,0xFF   ; rows 12-15
        .db     0x00,0xFF, 0xFF,0xFF, 0x00,0xFF, 0xFF,0xFF   ; rows 16-19
        .db     0x00,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF   ; rows 20-23
        .db     0x00,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF   ; rows 24-27
        .db     0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF   ; rows 28-31
        .db     0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF   ; rows 32-35
        .db     0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF   ; rows 36-39
        .db     0xFF,0xFF, 0xFF,0xFF, 0xAA,0xFF, 0xFF,0xFF   ; rows 40-43
        .db     0x55,0xFF, 0xFF,0xFF, 0xAA,0xFF, 0xAA,0xFF   ; rows 44-47
        .db     0x55,0xFF, 0xAA,0xFF, 0x55,0xFF, 0xAA,0xFF   ; rows 48-51
        .db     0x55,0xFF, 0x00,0xFF, 0x00,0xFF, 0x00,0xFF   ; rows 52-55
        .db     0x00,0xFF, 0x00,0xFF, 0x00,0xFF, 0x55,0xAA   ; rows 56-59
        .db     0xAA,0x55, 0x55,0xAA, 0xAA,0x55, 0x55,0xAA   ; rows 60-63

; A = pixel row -> HL = column base + 2*row. Clobbers DE.
rg_addr:
        add     a, a
        ld      e, a
        ld      d, #0
        ld      a, (_r_cb)
        ld      l, a
        ld      a, (_r_cb+1)
        ld      h, a
        add     hl, de
        ret

; B rows from HL: set this ray's 2 pixels to (_r_wl, _r_wh) under _r_m.
rg_rmw:
        ld      a, (_r_m)
        cpl
        ld      c, a
        ld      a, (_r_wl)
        ld      d, a
        ld      a, (_r_wh)
        ld      e, a
rm_loop:
        ld      a, (hl)
        and     c
        or      d
        ld      (hl+), a
        ld      a, (hl)
        and     c
        or      e
        ld      (hl+), a
        dec     b
        jr      NZ, rm_loop
        ret

; ---------------------------------------------------------------
; copy_vbuf: 2048 bytes _vbuf -> VRAM at _r_dst (0x8000 or 0x9000).
; Polls STAT before every 2-byte burst: once mode 0/1 is seen, VRAM stays
; open for >= 80 T (the rest of mode 0 plus all of mode 2), and both
; stores land within ~52 T. Works in any mode, sprites or not, and with
; the LCD off. The VBL ISR may run between poll and store, but it starts
; at LY 144 and ends well inside VBlank, so the stores stay safe.
; ---------------------------------------------------------------
_copy_vbuf::
        ld      hl, #_vbuf
        ld      a, (_r_dst)
        ld      e, a
        ld      a, (_r_dst+1)
        ld      d, a
        ld      b, #4                  ; 4 x 256 pairs
        ld      c, #0
cv_loop:
        ldh     a, (STATR)
        and     #0x02
        jr      NZ, cv_loop
        ld      a, (hl+)
        ld      (de), a
        inc     e                      ; pairs never straddle 256 (even dst)
        ld      a, (hl+)
        ld      (de), a
        inc     de
        dec     c
        jr      NZ, cv_loop
        dec     b
        jr      NZ, cv_loop
        ret
