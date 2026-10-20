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
; Side distances: sd = f * rcp[a] >> 8, f = distance to the first
; gridline (1/256 cell), a = |ray component|. In the log domain that is
; one add: antilog[log8[f] + lrc[a] - 16] (~2% error, like the old mul8).
; log8[f] only depends on the camera and the ray's sign, so render_asm
; fills r_lf once per image: x+, x-, y+, y-. f = 0 reads as f = 1
; (log8[0] = 0), 1/256 cell off - invisible.
; ---------------------------------------------------------------

; lf_pair: A = camera fraction (lo byte) -> A = log8[256 - lo] (dir +),
; B = log8[lo] (dir -). Clobbers DE, HL.
lf_pair:
        ld      e, a
        ld      d, #0
        ld      hl, #_log8
        add     hl, de
        ld      b, (hl)
        ld      a, e
        cpl
        inc     a                      ; 256 - lo, Z when lo == 0 (f = 256)
        jr      Z, 1$
        ld      e, a
        ld      hl, #_log8
        add     hl, de
        ld      a, (hl)
        ret
1$:     ld      a, #248                ; log8[256]
        ret

; SD_LU: A = log8[f], C = a -> HL = f * rcp[a] >> 8. Clobbers A, DE.
        .macro  SD_LU ?big
        ld      hl, #_lrc
        ld      e, c
        ld      d, #0
        add     hl, de
        add     a, (hl)
        ld      e, a
        ld      a, #0
        adc     a, a
        ld      d, a                   ; DE = index + 16 (0..481)
        jr      NZ, big
        ld      a, e
        cp      #16
        jr      NC, big
        ld      e, #16                 ; below 1/256 cell: antilog[0]
big:    ld      hl, #(_antilog - 32)
        add     hl, de
        add     hl, de
        ld      a, (hl+)
        ld      h, (hl)
        ld      l, a
        .endm

; OD_AVG arr, DE = k: arr[k] = (arr[k-1] + arr[k+1]) / 2. Keeps DE.
        .macro  OD_AVG arr
        ld      hl, #(arr - 1)
        add     hl, de
        ld      a, (hl+)
        inc     hl
        add     a, (hl)
        rra                            ; 9-bit sum / 2
        dec     hl
        ld      (hl), a
        .endm

; ---------------------------------------------------------------
; cast_ray: ray (_r_dx, _r_dy) from the camera -> _r_dist (perp, 8.8),
; _r_wall, _r_side, _r_hit (map cell address).
; ---------------------------------------------------------------
cast_ray:
        ; x: C = |dx|, sdx pushed, ddx = rcp[|dx|]
        ld      a, (_r_dx)
        ld      c, a
        ld      a, (_r_lf)
        bit     7, c
        jr      Z, 1$
        xor     a
        sub     c
        ld      c, a
        ld      a, (_r_lf+1)
1$:     SD_LU
        push    hl
        ld      l, c
        ld      h, #0
        add     hl, hl
        ld      de, #_rcp
        add     hl, de
        ld      a, (hl+)
        ld      (_r_ddx), a
        ld      a, (hl)
        ld      (_r_ddx+1), a
        ; y
        ld      a, (_r_dy)
        ld      c, a
        ld      a, (_r_lf+2)
        bit     7, c
        jr      Z, 2$
        xor     a
        sub     c
        ld      c, a
        ld      a, (_r_lf+3)
2$:     SD_LU
        push    hl
        ld      l, c
        ld      h, #0
        add     hl, hl
        ld      de, #_rcp
        add     hl, de
        ld      a, (hl+)
        ld      (_r_ddy), a
        ld      a, (hl)
        ld      (_r_ddy+1), a

        ld      a, (_r_map)            ; HL = camera's map cell
        ld      l, a
        ld      a, (_r_map+1)
        ld      h, a
        pop     de                     ; DE = sdy
        pop     bc                     ; BC = sdx

        ; one DDA loop per quadrant, so no sign tests per step. No step
        ; cap: mazegen walls rows/columns 0, 30 and 31, so every ray hits.
        ld      a, (_r_dy)
        rlca
        jr      C, 3$
        ld      a, (_r_dx)
        rlca
        jp      C, dda_mp
        jp      dda_pp
3$:     ld      a, (_r_dx)
        rlca
        jp      C, dda_mm
        jp      dda_pm

; DDA xop, ylo, yhi: BC = sdx, DE = sdy, HL = map cell. x step is
; xop hl, y step adds (yhi:ylo) = +-32 to HL. Exits to dda_hx / dda_hy
; with A = the cell hit.
        .macro  DDA xop, ylo, yhi
1$:     ld      a, b                   ; sdx < sdy ? step x : step y
        cp      d
        jr      C, 3$
        jr      NZ, 2$
        ld      a, c
        cp      e
        jr      C, 3$
2$:     ld      a, (_r_ddy)
        add     a, e
        ld      e, a
        ld      a, (_r_ddy+1)
        adc     a, d
        ld      d, a
        ld      a, l
        add     a, #ylo
        ld      l, a
        ld      a, h
        adc     a, #yhi
        ld      h, a
        ld      a, (hl)
        or      a
        jr      Z, 1$
        jp      dda_hy
3$:     ld      a, (_r_ddx)
        add     a, c
        ld      c, a
        ld      a, (_r_ddx+1)
        adc     a, b
        ld      b, a
        xop     hl
        ld      a, (hl)
        or      a
        jr      Z, 1$
        jp      dda_hx
        .endm

dda_pp: DDA     inc, 32, 0
dda_mp: DDA     dec, 32, 0
dda_pm: DDA     inc, 0xE0, 0xFF
dda_mm: DDA     dec, 0xE0, 0xFF

dda_hx:
        ld      (_r_wall), a
        ld      a, l
        ld      (_r_hit), a
        ld      a, h
        ld      (_r_hit+1), a
        xor     a
        ld      (_r_side), a
        ; dist = sdx - ddx
        ld      a, (_r_ddx)
        ld      e, a
        ld      a, c
        sub     e
        ld      (_r_dist), a
        ld      a, (_r_ddx+1)
        ld      e, a
        ld      a, b
        sbc     a, e
        ld      (_r_dist+1), a
        jr      dda_seen
dda_hy:
        ld      (_r_wall), a
        ld      a, l
        ld      (_r_hit), a
        ld      a, h
        ld      (_r_hit+1), a
        ld      a, #1
        ld      (_r_side), a
        ; dist = sdy - ddy
        ld      a, (_r_ddy)
        ld      c, a
        ld      a, e
        sub     c
        ld      (_r_dist), a
        ld      a, (_r_ddy+1)
        ld      c, a
        ld      a, d
        sbc     a, c
        ld      (_r_dist+1), a
; automap: seen[hit - imap] = 1 (HL = hit cell). Interpolated odd columns
; copy a neighbour's hit, so marking only cast rays covers every r_hits[k].
dda_seen:
        ld      a, l
        sub     #<_imap
        ld      e, a
        ld      a, h
        sbc     a, #>_imap
        ld      d, a
        ld      hl, #_seen
        add     hl, de
        ld      (hl), #1
        ret

; ---------------------------------------------------------------
; render_asm: 64 columns, then fill the 16 tile columns of _vbuf.
; Rays: even columns are always cast; an odd column whose neighbours hit
; the same map cell from the same side is interpolated (H is linear in
; screen x across a flat face, so the average is exact up to rounding),
; otherwise it is cast too. Column 63 is always cast.
; Per column k: r_top/r_bot (pixel rows), r_zb (perp distance in 1/8
; cells, sprite occlusion + fog), r_hits (cell, automap), r_wt (wall type
; 1..3 | side << 7), then r_clr from the colour pass.
; Colours: x-facing wall 0, y-facing wall 1, door 2; the first column on a
; new map cell or face draws its edge one shade darker (2, door 3).
; ---------------------------------------------------------------
_render_asm::
        ; per-image log8 of the camera's gridline fractions (see SD_LU)
        ld      a, (_cam_x)
        call    lf_pair
        ld      (_r_lf), a
        ld      a, b
        ld      (_r_lf+1), a
        ld      a, (_cam_y)
        call    lf_pair
        ld      (_r_lf+2), a
        ld      a, b
        ld      (_r_lf+3), a
        ; camera's map cell: imap + mapy*32 + mapx
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
        ld      a, l
        ld      (_r_map), a
        ld      a, h
        ld      (_r_map+1), a
        ; per-column ray direction hi bytes: r_dxs[k] = (rdx + k*stx) >> 8
        ; (the same 16-bit sums the old per-pass accumulators produced)
        ld      a, (_r_stx)
        ld      e, a
        ld      a, (_r_stx+1)
        ld      d, a
        ld      a, (_r_rdx)
        ld      l, a
        ld      a, (_r_rdx+1)
        ld      h, a
        ld      bc, #_r_dxs
        call    rd_tab
        ld      a, (_r_sty)
        ld      e, a
        ld      a, (_r_sty+1)
        ld      d, a
        ld      a, (_r_rdy)
        ld      l, a
        ld      a, (_r_rdy+1)
        ld      h, a
        ld      bc, #_r_dys
        call    rd_tab

        ; ---- even columns
        xor     a
        ld      (_r_k), a
ev_loop:
        call    ray_cs
        ld      a, (_r_k)
        add     a, #2
        ld      (_r_k), a
        cp      #64
        jr      C, ev_loop

        ; ---- odd columns
        ld      a, #1
        ld      (_r_k), a
od_loop:
        ld      a, (_r_k)
        cp      #63
        jr      Z, od_cast
        ld      e, a
        ld      d, #0
        ld      hl, #(_r_wt - 1)
        add     hl, de
        ld      a, (hl+)               ; wt[k-1]
        inc     hl
        cp      (hl)                   ; wt[k+1]
        jr      NZ, od_cast
        ld      hl, #(_r_hits - 2)
        add     hl, de
        add     hl, de
        ld      a, (hl+)
        ld      c, a                   ; hits[k-1]
        ld      a, (hl+)
        ld      b, a
        inc     hl
        inc     hl
        ld      a, (hl+)               ; hits[k+1]
        cp      c
        jr      NZ, od_cast
        ld      a, (hl)
        cp      b
        jr      NZ, od_cast
        ; same cell and face: copy hits/wt, average top/bot/zb
        ld      hl, #_r_hits
        add     hl, de
        add     hl, de
        ld      a, c
        ld      (hl+), a
        ld      (hl), b
        ld      hl, #(_r_wt - 1)
        add     hl, de
        ld      a, (hl+)
        ld      (hl), a
        OD_AVG  _r_top
        OD_AVG  _r_bot
        OD_AVG  _r_zb
        ld      hl, #(_r_clr + 1)
        add     hl, de
        ld      a, (hl-)
        ld      (hl), a
        jr      od_next
od_cast:
        call    ray_cs                 ; keeps DE = k
        call    ed_cmp                 ; edge between k-1 and k
        ld      a, e
        cp      #63
        jr      Z, od_next
        inc     de
        call    ed_cmp                 ; edge between k and k+1
od_next:
        ld      a, (_r_k)
        add     a, #2
        ld      (_r_k), a
        cp      #64
        jr      C, od_loop

        ; ---- colour pass. r_clr = left | right << 2 (2-bit colours):
        ; x-facing wall 0, y-facing wall 1, door 2; the first column on a new
        ; map cell or face (edge) one shade darker: wall 2, door 3. Walls
        ; (not edges) fade with r_zb: right pixel one shade darker from
        ; FOG_NEAR cells, both from FOG_FAR (1-px vertical dither); doors and
        ; edges are left alone so nothing fades into the black horizon band.
        ; The ray passes already left the code without fog in r_clr: ray_cs
        ; stores the base (wall side * 5 = 0/5, door 10) and ed_cmp turns an
        ; edge into 10 (wall) / 15 (door). Fog: 5s -> 5s+4 (near), 5s+5 (far).
        ld      hl, #_r_clr
        ld      de, #_r_zb
        ld      c, #64
cl_loop:
        ld      a, (hl)
        cp      #10
        jr      NC, cl_next            ; door or edge
        ld      b, a
        ld      a, (de)
        cp      #(FOG_NEAR * 8)
        jr      C, cl_next
        cp      #(FOG_FAR * 8)
        ccf                            ; C = far
        ld      a, b
        adc     a, #4
        ld      (hl), a
cl_next:
        inc     hl
        inc     de
        dec     c
        jr      NZ, cl_loop
        jp      fill_go

; DE = j (1..63): if column j-1 and j differ in wall type/side or map cell
; (a new face starts at j), darken r_clr[j]'s base: wall -> 10, door -> 15.
; Keeps BC, DE.
ed_cmp:
        ld      hl, #(_r_wt - 1)
        add     hl, de
        ld      a, (hl+)
        cp      (hl)
        jr      NZ, ed_dark
        ld      hl, #(_r_hits - 2)
        add     hl, de
        add     hl, de
        ld      a, (hl+)               ; hits[j-1] lo
        inc     hl
        cp      (hl)                   ; hits[j] lo
        jr      NZ, ed_dark
        dec     hl
        ld      a, (hl+)               ; hits[j-1] hi
        inc     hl
        cp      (hl)                   ; hits[j] hi
        ret     Z
ed_dark:
        ld      hl, #_r_clr
        add     hl, de
        ld      a, (hl)
        cp      #10
        ld      a, #10
        jr      C, 1$
        ld      a, #15
1$:     ld      (hl), a
        ret

; Cast column _r_k along (r_dxs[k], r_dys[k]); store top/bot/zb/hits/wt.
ray_cs:
        ld      a, (_r_k)
        ld      e, a
        ld      d, #0
        ld      hl, #_r_dxs
        add     hl, de
        ld      a, (hl)
        ld      (_r_dx), a
        ld      hl, #_r_dys
        add     hl, de
        ld      a, (hl)
        ld      (_r_dy), a
        call    cast_ray
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

        ld      hl, #_r_hits           ; automap: the cell this ray saw
        add     hl, de
        add     hl, de
        ld      a, (_r_hit)
        ld      (hl+), a
        ld      a, (_r_hit+1)
        ld      (hl), a
        ld      a, (_r_side)
        rrca                           ; side -> bit 7
        ld      b, a
        ld      a, (_r_wall)
        or      b
        ld      hl, #_r_wt
        add     hl, de
        ld      (hl), a
        ; base colour code (see the colour pass): door 10, else side * 5
        ld      hl, #_r_clr
        add     hl, de
        and     #3
        cp      #3
        ld      a, #10
        jr      Z, 1$
        xor     a
        bit     7, b
        jr      Z, 1$
        ld      a, #5
1$:     ld      (hl), a
        ret

; HL = 8.8 start, DE = step, BC = 64-byte table: table[k] = (HL + k*DE) >> 8
rd_tab:
        .rept   64
        ld      a, h
        ld      (bc), a
        inc     bc
        add     hl, de
        .endm
        ret

; Ragged-patch rays (f_rag). DE walks r_top / r_bot, C = maxtop / minbot,
; M = the ray's 2-pixel mask. RTOP: rows [top, maxtop) upwards from r_pt
; (hi byte of row maxtop-1). RBOT: rows [minbot, bot) from r_cb (row minbot).
        .macro  RTOP M, ?skip, ?loop
        ld      a, (de)
        inc     de
        sub     c                      ; top - maxtop (<= 0)
        jr      Z, skip
        cpl
        inc     a
        ld      b, a                   ; rows
        push    bc
        push    de
        ld      a, (_r_lo)
        and     #M
        ld      d, a
        ld      a, (_r_hi)
        and     #M
        ld      e, a
        ld      c, #(~M & 0xFF)
        ld      a, (_r_pt)
        ld      l, a
        ld      a, (_r_pt+1)
        ld      h, a
loop:   ld      a, (hl)
        and     c
        or      e
        ld      (hl-), a
        ld      a, (hl)
        and     c
        or      d
        ld      (hl-), a
        dec     b
        jr      NZ, loop
        pop     de
        pop     bc
skip:
        .endm

        .macro  RBOT M, ?skip, ?loop
        ld      a, (de)
        inc     de
        sub     c                      ; bot - minbot (>= 0)
        jr      Z, skip
        ld      b, a                   ; rows
        push    bc
        push    de
        ld      a, (_r_lo)
        and     #M
        ld      d, a
        ld      a, (_r_hi)
        and     #M
        ld      e, a
        ld      c, #(~M & 0xFF)
        ld      a, (_r_cb)
        ld      l, a
        ld      a, (_r_cb+1)
        ld      h, a
loop:   ld      a, (hl)
        and     c
        or      d
        ld      (hl+), a
        ld      a, (hl)
        and     c
        or      e
        ld      (hl+), a
        dec     b
        jr      NZ, loop
        pop     de
        pop     bc
skip:
        .endm

; ---- fill: per tile column (4 rays), rows [0,maxtop) ceiling,
; [maxtop,minbot) all-wall, [minbot,64) floor, then patch each ray's
; ragged ends with read-modify-write. Every top <= 31 < 33 <= every
; bot (ht >= 2), so the all-wall core is never empty.
fill_go:
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
        .rept   4
        ld      a, (hl+)
        rrca                           ; left lo
        rl      c
        rrca                           ; left hi
        rl      e
        rrca                           ; right lo
        rl      c
        rrca                           ; right hi
        rl      e
        .endm
        ld      a, c
        ld      (_r_lo), a
        ld      a, e
        ld      (_r_hi), a

        ; ceiling: rows [maxtop-1 .. 0], unrolled (ceil_blk)
        ld      a, (_r_mxt)
        add     a, a
        ld      e, a
        ld      d, #0
        ld      hl, #ceil_tab
        add     hl, de
        ld      a, (hl+)
        ld      b, (hl)
        ld      c, a                   ; BC = entry for maxtop
        ld      a, (_r_cb)
        ld      l, a
        ld      a, (_r_cb+1)
        ld      h, a
        add     hl, de                 ; HL = cb + 2*maxtop (core start)
        push    hl
        dec     hl                     ; hi byte of row maxtop-1
        ld      a, l
        ld      (_r_pt), a             ; top patches start here
        ld      a, h
        ld      (_r_pt+1), a
        call    bg_go
        pop     hl
f_core:
        ; rows [maxtop, minbot): enter the unrolled fc_blk N = minbot-maxtop
        ; rows (3 bytes each) before fc_end; it falls through to fc_end.
        ld      a, (_r_mxt)
        ld      c, a
        ld      a, (_r_mnb)
        sub     c
        ld      c, a
        add     a, a
        add     a, c                   ; 3N (<= 192)
        ld      c, a
        ld      a, #<fc_end
        sub     c
        ld      c, a
        ld      a, #>fc_end
        sbc     a, #0
        ld      b, a
        push    bc
        ld      a, (_r_hi)
        ld      e, a
        ld      a, (_r_lo)
        ret
fc_blk:
        .rept   64
        ld      (hl+), a
        ld      (hl), e
        inc     hl
        .endm
fc_end:
        ld      a, l
        ld      (_r_cb), a             ; row minbot: bottom patches start here
        ld      a, h
        ld      (_r_cb+1), a
        ; floor: rows [minbot .. 63], unrolled (floor_blk)
        ld      a, (_r_mnb)
        sub     #32
        add     a, a
        ld      e, a
        ld      d, #0
        push    hl
        ld      hl, #floor_tab
        add     hl, de
        ld      a, (hl+)
        ld      b, (hl)
        ld      c, a                   ; BC = entry for minbot
        pop     hl
        call    bg_go

f_rag:
        ; ragged ends: ray j's rows [top, maxtop) and [minbot, bot) get its
        ; 2 pixels in wall colour (r_lo/r_hi under its mask) by read-modify-
        ; write. Rays with top == maxtop / bot == minbot cost 32 T each.
        ld      a, (_r_tc)
        add     a, a
        add     a, a
        ld      e, a
        ld      d, #0
        push    de
        ld      hl, #_r_top
        add     hl, de
        ld      d, h
        ld      e, l                   ; DE = &top[4*tc]
        ld      a, (_r_mxt)
        ld      c, a
        RTOP    0xC0
        RTOP    0x30
        RTOP    0x0C
        RTOP    0x03
        pop     de
        ld      hl, #_r_bot
        add     hl, de
        ld      d, h
        ld      e, l                   ; DE = &bot[4*tc]
        ld      a, (_r_mnb)
        ld      c, a
        RBOT    0xC0
        RBOT    0x30
        RBOT    0x0C
        RBOT    0x03

        ld      a, (_r_tc)
        inc     a
        ld      (_r_tc), a
        cp      #16
        jp      NZ, fill_col

; one mortar column test: HL walks r_wt, C = k
        .macro  MOK M
        ld      a, (hl+)
        and     #3
        cp      #2
        ld      b, #M
        call    Z, mo_col
        inc     c
        .endm
; one mortar row at HL: B = mask, C = ~mask; lo &= ~mask, hi |= mask
        .macro  MOR
        ld      a, (hl)
        and     c
        ld      (hl+), a
        ld      a, (hl)
        or      b
        ld      (hl-), a
        .endm

; ---- '=' walls: dark mortar rows at 1/4, 1/2 and 3/4 of the wall height
; (fractions of the projected height, so they converge with perspective).
        ld      hl, #_r_wt
        ld      c, #0                  ; C = k
mo_loop:
        MOK     0xC0
        MOK     0x30
        MOK     0x0C
        MOK     0x03
        bit     6, c
        jr      Z, mo_loop
        ret

; B = mask of column C = k (an '=' wall): its 2 pixels -> colour 2 on rows
; top + H/4, + H/2, + 3H/4 (H >= 8). Keeps BC, HL.
mo_col:
        push    hl
        ld      e, c
        ld      d, #0
        ld      hl, #_r_bot
        add     hl, de
        ld      a, (hl)
        ld      hl, #_r_top
        add     hl, de
        ld      d, (hl)                ; D = top
        sub     d                      ; H
        cp      #8
        jr      C, mo_ret              ; too small to show bricks
        and     #0xFC                  ; clears carry
        rra
        ld      e, a                   ; E = 2 * (H / 4): bytes per step
        add     a, d
        add     a, d
        ld      l, a                   ; 2 * (top + H/4) (< 128)
        ld      a, c
        rrca
        rrca
        rrca                           ; (k & 7) << 5 | k >> 3
        ld      h, a
        and     #0x80                  ; (k >> 2 & 1) << 7
        or      l
        ld      l, a
        ld      a, h
        and     #7                     ; k >> 3
        ld      h, a
        ld      a, l
        add     a, #<_vbuf
        ld      l, a
        ld      a, h
        adc     a, #>_vbuf
        ld      h, a                   ; HL = vbuf + (k>>2)*128 + 2*(top+H/4)
        ld      d, #0
        ld      a, b
        cpl
        push    bc
        ld      c, a                   ; C = ~mask
        MOR
        add     hl, de
        MOR
        add     hl, de
        MOR
        pop     bc
mo_ret:
        pop     hl
        ret

; Jump to the ceiling/floor block entry in BC with the register contract
; A=0xFF B=0xAA C=0x55 D=0x00; the block's ret returns to bg_go's caller.
; Pattern per pixel row (lo, hi): row 0 = top (nearest ceiling), 32 =
; horizon, 63 = nearest floor. Ceiling: solid 2, 2/3 horizontal lines,
; sparse lines, black at the horizon. Floor: black, 25% dots, 2/3 checker,
; solid 2, 1/2 checker at the bottom.
bg_go:
        push    bc
        ld      a, #0xFF
        ld      bc, #0xAA55
        ld      d, #0x00
        ret

; ---- generated by tools/gen_bgtab.py (python3 tools/gen_bgtab.py) ----
; ceiling entry for maxtop k = code for row k-1 (rows run k-1 .. 0)
ceil_tab:
        .dw     cr_end
        .dw     cr0
        .dw     cr1
        .dw     cr2
        .dw     cr3
        .dw     cr4
        .dw     cr5
        .dw     cr6
        .dw     cr7
        .dw     cr8
        .dw     cr9
        .dw     cr10
        .dw     cr11
        .dw     cr12
        .dw     cr13
        .dw     cr14
        .dw     cr15
        .dw     cr16
        .dw     cr17
        .dw     cr18
        .dw     cr19
        .dw     cr20
        .dw     cr21
        .dw     cr22
        .dw     cr23
        .dw     cr24
        .dw     cr25
        .dw     cr26
        .dw     cr27
        .dw     cr28
        .dw     cr29
        .dw     cr30
        .dw     cr31
; floor entry for minbot k (32..64) = code for row k (rows run k .. 63)
floor_tab:
        .dw     fr32
        .dw     fr33
        .dw     fr34
        .dw     fr35
        .dw     fr36
        .dw     fr37
        .dw     fr38
        .dw     fr39
        .dw     fr40
        .dw     fr41
        .dw     fr42
        .dw     fr43
        .dw     fr44
        .dw     fr45
        .dw     fr46
        .dw     fr47
        .dw     fr48
        .dw     fr49
        .dw     fr50
        .dw     fr51
        .dw     fr52
        .dw     fr53
        .dw     fr54
        .dw     fr55
        .dw     fr56
        .dw     fr57
        .dw     fr58
        .dw     fr59
        .dw     fr60
        .dw     fr61
        .dw     fr62
        .dw     fr63
        .dw     fr_end
; ceiling block: HL = hi byte of row maxtop-1, writes backwards
ceil_blk:
cr31:
        ld      (hl-), a
        ld      (hl-), a
cr30:
        ld      (hl-), a
        ld      (hl-), a
cr29:
        ld      (hl-), a
        ld      (hl-), a
cr28:
        ld      (hl-), a
        ld      (hl-), a
cr27:
        ld      (hl-), a
        ld      (hl-), a
cr26:
        ld      (hl-), a
        ld      (hl-), a
cr25:
        ld      (hl-), a
        ld      (hl-), a
cr24:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr23:
        ld      (hl-), a
        ld      (hl-), a
cr22:
        ld      (hl-), a
        ld      (hl-), a
cr21:
        ld      (hl-), a
        ld      (hl-), a
cr20:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr19:
        ld      (hl-), a
        ld      (hl-), a
cr18:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr17:
        ld      (hl-), a
        ld      (hl-), a
cr16:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr15:
        ld      (hl-), a
        ld      (hl-), a
cr14:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr13:
        ld      (hl-), a
        ld      (hl-), a
cr12:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr11:
        ld      (hl-), a
        ld      (hl-), a
cr10:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr9:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr8:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr7:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr6:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr5:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr4:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr3:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr2:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr1:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr0:
        ld      (hl-), a
        ld      (hl), d
        dec     hl
cr_end:
        ret
; floor block: HL = lo byte of row minbot, writes forwards
floor_blk:
fr32:
        ld      (hl+), a
        ld      (hl+), a
fr33:
        ld      (hl+), a
        ld      (hl+), a
fr34:
        ld      (hl+), a
        ld      (hl+), a
fr35:
        ld      (hl+), a
        ld      (hl+), a
fr36:
        ld      (hl+), a
        ld      (hl+), a
fr37:
        ld      (hl+), a
        ld      (hl+), a
fr38:
        ld      (hl+), a
        ld      (hl+), a
fr39:
        ld      (hl+), a
        ld      (hl+), a
fr40:
        ld      (hl+), a
        ld      (hl+), a
fr41:
        ld      (hl+), a
        ld      (hl+), a
fr42:
        ld      (hl), b
        inc     hl
        ld      (hl+), a
fr43:
        ld      (hl+), a
        ld      (hl+), a
fr44:
        ld      (hl), c
        inc     hl
        ld      (hl+), a
fr45:
        ld      (hl+), a
        ld      (hl+), a
fr46:
        ld      (hl), b
        inc     hl
        ld      (hl+), a
fr47:
        ld      (hl), b
        inc     hl
        ld      (hl+), a
fr48:
        ld      (hl), c
        inc     hl
        ld      (hl+), a
fr49:
        ld      (hl), b
        inc     hl
        ld      (hl+), a
fr50:
        ld      (hl), c
        inc     hl
        ld      (hl+), a
fr51:
        ld      (hl), b
        inc     hl
        ld      (hl+), a
fr52:
        ld      (hl), c
        inc     hl
        ld      (hl+), a
fr53:
        ld      (hl), d
        inc     hl
        ld      (hl+), a
fr54:
        ld      (hl), d
        inc     hl
        ld      (hl+), a
fr55:
        ld      (hl), d
        inc     hl
        ld      (hl+), a
fr56:
        ld      (hl), d
        inc     hl
        ld      (hl+), a
fr57:
        ld      (hl), d
        inc     hl
        ld      (hl+), a
fr58:
        ld      (hl), d
        inc     hl
        ld      (hl+), a
fr59:
        ld      (hl), c
        inc     hl
        ld      (hl), b
        inc     hl
fr60:
        ld      (hl), b
        inc     hl
        ld      (hl), c
        inc     hl
fr61:
        ld      (hl), c
        inc     hl
        ld      (hl), b
        inc     hl
fr62:
        ld      (hl), b
        inc     hl
        ld      (hl), c
        inc     hl
fr63:
        ld      (hl), c
        inc     hl
        ld      (hl), b
        inc     hl
fr_end:
        ret
; ---- end generated ----

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

; ---------------------------------------------------------------
; copy_view: like copy_vbuf (same STAT-poll + store body), but per tile
; column only rows [t, b): t/b = the union of this image's span (min top,
; max bot of the column's 4 rays, read from r_top/r_bot, still valid
; after render_asm) and the span last copied into this bank (r_vh, set
; by C's view_reset to [0, 64) = copy all). Rows outside a span are pure
; ceiling/floor pattern, the same in every column and image (fill writes
; rows [0,mintop) and [maxbot,64) only from ceil_blk/floor_blk; ragged
; patches and mortar stay inside [top, bot)), so they are already right
; in VRAM. r_vh: [0..15] bank 0x8000 tops, [16..31] bank 0x9000 tops,
; [32..63] the bots. Uses r_tc (fill's, free by now).
; ---------------------------------------------------------------
_copy_view::
        xor     a
        ld      (_r_tc), a
cw_col:
        ld      a, (_r_tc)
        add     a, a
        add     a, a
        ld      e, a
        ld      d, #0                  ; DE = 4*tc
        ld      hl, #_r_top            ; C = min top
        add     hl, de
        ld      a, (hl+)
        cp      (hl)
        jr      C, 1$
        ld      a, (hl)
1$:     inc     hl
        cp      (hl)
        jr      C, 2$
        ld      a, (hl)
2$:     inc     hl
        cp      (hl)
        jr      C, 3$
        ld      a, (hl)
3$:     ld      c, a
        ld      hl, #_r_bot            ; B = max bot
        add     hl, de
        ld      a, (hl+)
        cp      (hl)
        jr      NC, 4$
        ld      a, (hl)
4$:     inc     hl
        cp      (hl)
        jr      NC, 5$
        ld      a, (hl)
5$:     inc     hl
        cp      (hl)
        jr      NC, 6$
        ld      a, (hl)
6$:     ld      b, a
        ; history: r_vh[bank*16 + tc] (top), +32 (bot); store new, merge old
        ld      a, (_r_dst+1)
        and     #0x10                  ; 0x80 -> 0, 0x90 -> 16
        ld      e, a
        ld      a, (_r_tc)
        add     a, e
        ld      e, a                   ; D = 0
        ld      hl, #_r_vh
        add     hl, de
        ld      a, (hl)
        ld      (hl), c
        cp      c
        jr      NC, 7$
        ld      c, a                   ; C = min(new, old) top
7$:     ld      de, #32
        add     hl, de
        ld      a, (hl)
        ld      (hl), b
        cp      b
        jr      C, 8$
        ld      b, a                   ; B = max(new, old) bot
8$:     ld      a, b
        sub     c
        ld      b, a                   ; B = pairs (> 0: top <= 31 < 33 <= bot)
        ; DE = tc*128 + 2*t (2*t <= 126, no carry into D)
        ld      a, (_r_tc)
        srl     a
        ld      d, a
        ld      a, #0
        rra
        ld      e, a
        ld      a, c
        add     a, a
        add     a, e
        ld      e, a
        ld      hl, #_vbuf
        add     hl, de
        push    hl
        ld      a, (_r_dst)
        ld      l, a
        ld      a, (_r_dst+1)
        ld      h, a
        add     hl, de
        ld      d, h
        ld      e, l                   ; DE = dst (even)
        pop     hl                     ; HL = src
cw_loop:
        ldh     a, (STATR)
        and     #0x02
        jr      NZ, cw_loop
        ld      a, (hl+)
        ld      (de), a
        inc     e                      ; pairs never straddle 256 (even dst)
        ld      a, (hl+)
        ld      (de), a
        inc     de
        dec     b
        jr      NZ, cw_loop
        ld      a, (_r_tc)
        inc     a
        ld      (_r_tc), a
        cp      #16
        jp      NZ, cw_col
        ret
