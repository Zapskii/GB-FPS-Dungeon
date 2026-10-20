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
FOG_NEAR = 3                    ; cells (perp hi byte): wall starts to dither
FOG_FAR  = 6                    ; cells: wall one full shade darker


; Per-ray arrays at 64-byte strides from r_top (ray_end walks them with one
; pointer), then the ray direction tables, then imap with seen[] right after
; it (seen[cell - imap] = cell + 1024). main.c declares these extern.
        .area   _DATA
_r_top::        .ds     64
_r_bot::        .ds     64
_r_zb::         .ds     64
_r_wt::         .ds     64
_r_clr::        .ds     64
_r_hits::       .ds     128
_r_dxs::        .ds     64
_r_dys::        .ds     64
_imap::         .ds     1024
_seen::         .ds     1024
c_desc:         .ds     8*16           ; per tile column, for copy_view: rows T,
                                       ; core start, core end, B (pieces between
                                       ; them), lo, hi, min top, max bot
r_uni:          .ds     1              ; fill_col: this column's core is uniform

; per-image and per-ray scalars, ldh-addressable
        .area   _HRAM
_h_lf::         .ds     4              ; log8 gridline fractions: x+, x-, y+, y-
_h_map::        .ds     2              ; camera's map cell
_h_ddx::        .ds     2              ; this ray's one-cell steps
_h_ddy::        .ds     2
_h_k::          .ds     1              ; this ray's column
_h_mnt::        .ds     1              ; fill_col: the column's min top
_h_mxb::        .ds     1              ;   and max bot
_h_gmx::        .ds     1              ; fill: highest wall top in the image

        .area   _CODE

; ---------------------------------------------------------------
; Side distances: sd = f * rcp[a] >> 8, f = distance to the first
; gridline (1/256 cell), a = |ray component|. In the log domain that is
; one add: antilog[log8[f] + lrc[a] - 16] (~2% error, like the old mul8).
; log8[f] only depends on the camera and the ray's sign, so render_asm
; fills h_lf once per image: x+, x-, y+, y-. f = 0 reads as f = 1
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

; SD_LU: A = log8[f], C = a -> HL = f * rcp[a] >> 8. The 9-bit sum
; log8[f] + lrc[a] indexes al_lo/al_hi (rtabs.s) as L = low byte,
; H = page + carry; the tables already clamp sums below 16. Clobbers A.
        .macro  SD_LU
        ld      h, #>_lrc
        ld      l, c
        add     a, (hl)
        ld      l, a
        ld      a, #>_al_lo
        adc     a, #0
        ld      h, a
        ld      a, (hl)
        inc     h
        inc     h                      ; al_hi = al_lo + 512
        ld      h, (hl)
        ld      l, a
        .endm

; RCPH dst: C = a -> HRAM dst = rcp[a]. Clobbers A, HL.
        .macro  RCPH dst
        ld      h, #>_rcp_lo           ; rcp_lo = page + 0x80, rcp_hi one page on
        ld      a, c
        or      #0x80
        ld      l, a
        ld      a, (hl)
        ldh     (dst), a
        inc     h
        ld      a, (hl)
        ldh     (dst+1), a
        .endm

; OD_AVG2 arr, DE = k: arr[k] = (arr[k-2] + arr[k+2]) / 2. Keeps DE.
        .macro  OD_AVG2 arr
        ld      hl, #(arr - 2)
        add     hl, de
        ld      a, (hl+)
        inc     hl
        inc     hl
        inc     hl
        add     a, (hl)
        rra                            ; 9-bit sum / 2
        dec     hl
        dec     hl
        ld      (hl), a
        .endm

; ---------------------------------------------------------------
; ray_cs: cast column _r_k along (r_dxs[k], r_dys[k]). Stores r_top,
; r_bot, r_zb (perp in 1/8 cells), r_wt, r_clr (base colour), r_hits and
; marks seen[]. Returns DE = k.
; ---------------------------------------------------------------
ray_cs:
        ld      a, (_r_k)
        ldh     (_h_k), a
        ld      e, a
        ld      d, #0
        ld      hl, #_r_dxs
        add     hl, de
        ld      e, (hl)                ; E = dx
        ld      bc, #64                ; r_dys follows r_dxs
        add     hl, bc
        ld      b, (hl)                ; B = dy
        ; x: sdx (pushed) and ddx from C = |dx|
        ld      c, e
        ldh     a, (_h_lf)
        bit     7, e
        jr      Z, 1$
        xor     a
        sub     e
        ld      c, a
        ldh     a, (_h_lf+1)
1$:     SD_LU
        push    hl
        RCPH    _h_ddx
        ; y
        ld      c, b
        ldh     a, (_h_lf+2)
        bit     7, b
        jr      Z, 2$
        xor     a
        sub     b
        ld      c, a
        ldh     a, (_h_lf+3)
2$:     SD_LU
        push    hl
        RCPH    _h_ddy

        ; one DDA loop per quadrant, so no sign tests per step. No step
        ; cap: mazegen walls rows/columns 0, 30 and 31, so every ray hits.
        ldh     a, (_h_map)
        ld      l, a
        ldh     a, (_h_map+1)
        ld      h, a                   ; HL = camera's map cell
        bit     7, b
        jr      NZ, 3$
        bit     7, e
        pop     de                     ; DE = sdy (pops keep the flags)
        pop     bc                     ; BC = sdx
        jp      NZ, dda_mp
        jp      dda_pp
3$:     bit     7, e
        pop     de
        pop     bc
        jp      NZ, dda_mm
        jp      dda_pm

; DDA xop, ylo, ycc, hop: BC = sdx, DE = sdy, HL = map cell. x step is
; xop hl, y step adds ylo (+-32) to L and fixes H with hop unless ycc.
; Exits to dda_hx / dda_hy with A = the cell hit.
        .macro  DDA xop, ylo, ycc, hop, ?ny
1$:     ld      a, b                   ; sdx < sdy ? step x : step y
        cp      d
        jr      C, 3$
        jr      NZ, 2$
        ld      a, c
        cp      e
        jr      C, 3$
2$:     ldh     a, (_h_ddy)
        add     a, e
        ld      e, a
        ldh     a, (_h_ddy+1)
        adc     a, d
        ld      d, a
        ld      a, l
        add     a, #ylo
        ld      l, a
        jr      ycc, ny
        hop     h
ny:     ld      a, (hl)
        or      a
        jr      Z, 1$
        jp      dda_hy
3$:     ldh     a, (_h_ddx)
        add     a, c
        ld      c, a
        ldh     a, (_h_ddx+1)
        adc     a, b
        ld      b, a
        xop     hl
        ld      a, (hl)
        or      a
        jr      Z, 1$
        jp      dda_hx
        .endm

dda_pp: DDA     inc, 32, NC, inc
dda_mp: DDA     dec, 32, NC, inc
dda_pm: DDA     inc, 0xE0, C, dec
dda_mm: DDA     dec, 0xE0, C, dec

; DDA exits: A = cell hit (wall type 1..3, 3 = door), HL = its address.
; perp = sd - dd of the axis just stepped.
dda_hx:
        push    hl                     ; hit cell
        ld      l, a                   ; wt = cell (side 0)
        ldh     a, (_h_ddx)
        ld      h, a
        ld      a, c
        sub     h
        ld      e, a
        ldh     a, (_h_ddx+1)
        ld      h, a
        ld      a, b
        sbc     a, h
        ld      d, a                   ; DE = sdx - ddx
        ld      a, l
        ld      c, #0                  ; base colour: x-facing 0, door 10
        cp      #3
        jr      NZ, ray_end
        ld      c, #10
        jr      ray_end
dda_hy:
        push    hl
        or      #0x80
        ld      l, a                   ; wt = cell | side << 7
        ldh     a, (_h_ddy)
        ld      h, a
        ld      a, e
        sub     h
        ld      e, a
        ldh     a, (_h_ddy+1)
        ld      h, a
        ld      a, d
        sbc     a, h
        ld      d, a                   ; DE = sdy - ddy
        ld      a, l
        ld      c, #5                  ; y-facing 5, door 10
        cp      #0x83
        jr      NZ, ray_end
        ld      c, #10
; A = wt, C = base colour, DE = perp (8.8), (SP) = hit cell.
ray_end:
        ld      b, a
        push    bc
        ; H = ht[min(perp >> 3, 1023)], zb = min(perp >> 5, 255)
        ld      a, d
        cp      #0x20
        jr      NC, 1$
        srl     d
        rr      e
        srl     d
        rr      e
        srl     d
        rr      e                      ; DE = perp >> 3
        ld      hl, #_ht
        add     hl, de
        ld      c, (hl)                ; C = H (2..64)
        ld      a, e
        srl     d
        rra
        srl     d
        rra                            ; A = perp >> 5 (D <= 3)
        jr      2$
1$:     ld      a, (_ht + 1023)
        ld      c, a
        ld      a, #0xFF
2$:     ld      b, a                   ; B = zb
        ld      a, #64
        sub     c
        srl     a
        ld      e, a                   ; E = top
        add     a, c
        ld      d, a                   ; D = bot
        ldh     a, (_h_k)
        ld      hl, #_r_top
        add     a, l
        ld      l, a
        adc     a, h
        sub     l
        ld      h, a                   ; HL = &r_top[k]
        ld      (hl), e
        ld      a, d
        ld      de, #64
        add     hl, de
        ld      (hl), a                ; r_bot
        add     hl, de
        ld      (hl), b                ; r_zb
        pop     bc
        add     hl, de
        ld      (hl), b                ; r_wt
        add     hl, de
        ld      (hl), c                ; r_clr
        add     hl, de                 ; r_hits + k, + k again = &r_hits[k]
        ldh     a, (_h_k)
        ld      e, a                   ; DE = k
        add     hl, de
        pop     bc                     ; hit cell
        ld      (hl), c
        inc     hl
        ld      (hl), b
        ; automap: seen[hit - imap] = 1. Interpolated odd columns copy a
        ; neighbour's hit, so marking only cast rays covers every r_hits[k].
        ld      a, b
        add     a, #4                  ; + 1024
        ld      b, a
        ld      a, #1
        ld      (bc), a
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
        ldh     (_h_lf), a
        ld      a, b
        ldh     (_h_lf+1), a
        ld      a, (_cam_y)
        call    lf_pair
        ldh     (_h_lf+2), a
        ld      a, b
        ldh     (_h_lf+3), a
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
        ldh     (_h_map), a
        ld      a, h
        ldh     (_h_map+1), a
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

        ; ---- every 4th column, and 62
        xor     a
        ld      (_r_k), a
ev_loop:
        call    ray_cs
        ld      a, (_r_k)
        add     a, #4
        ld      (_r_k), a
        cp      #64
        jr      C, ev_loop
        ld      a, #62
        ld      (_r_k), a
        call    ray_cs

        ; ---- columns 2, 6 .. 58: as the odd columns below, from k-2 and
        ; k+2. Two rays that hit the same face of one cell leave no room for
        ; another cell between them, so the rays between hit it too.
        ld      a, #2
        ld      (_r_k), a
e2_loop:
        ld      a, (_r_k)
        ld      e, a
        ld      d, #0
        ld      hl, #(_r_wt - 2)
        add     hl, de
        ld      a, (hl+)               ; wt[k-2]
        inc     hl
        inc     hl
        inc     hl
        cp      (hl)                   ; wt[k+2]
        jr      NZ, e2_cast
        ld      hl, #(_r_hits - 4)
        add     hl, de
        add     hl, de
        ld      a, (hl+)
        ld      c, a                   ; hits[k-2]
        ld      a, (hl+)
        ld      b, a
        inc     hl
        inc     hl
        inc     hl
        inc     hl
        inc     hl
        inc     hl
        ld      a, (hl+)               ; hits[k+2]
        cp      c
        jr      NZ, e2_cast
        ld      a, (hl)
        cp      b
        jr      NZ, e2_cast
        ld      hl, #_r_hits
        add     hl, de
        add     hl, de
        ld      a, c
        ld      (hl+), a
        ld      (hl), b
        ld      hl, #(_r_wt - 2)
        add     hl, de
        ld      a, (hl+)
        inc     hl
        ld      (hl), a
        OD_AVG2 _r_top
        OD_AVG2 _r_bot
        OD_AVG2 _r_zb
        ld      hl, #(_r_clr + 2)
        add     hl, de
        ld      a, (hl-)
        dec     hl
        ld      (hl), a
        jr      e2_next
e2_cast:
        call    ray_cs
e2_next:
        ld      a, (_r_k)
        add     a, #4
        ld      (_r_k), a
        cp      #62
        jr      C, e2_loop

        ; ---- odd columns. DE = k (ray_cs returns it, ed_cmp keeps it),
        ; BC = the 64-byte array stride, and the averages/wt/clr come from
        ; one pointer walk from r_top instead of one address per array.
        ; wt[k], clr[k], hits[k] and the averages are written before the
        ; checks: if a check fails, ray_cs rewrites every per-column value.
        ld      bc, #64
        ld      de, #1                 ; k
od_loop:
        ld      hl, #_r_top
        add     hl, de
        ; top, bot, zb: [k] = ([k-1] + [k+1]) / 2
        .rept   3
        dec     hl
        ld      a, (hl+)
        inc     hl
        add     a, (hl)
        rra
        dec     hl
        ld      (hl), a
        add     hl, bc
        .endm
        ; wt[k] = wt[k-1] unless the faces differ
        dec     hl                     ; &wt[k-1]
        ld      a, (hl+)
        ld      (hl), a                ; wt[k]
        inc     hl                     ; &wt[k+1]
        cp      (hl)
        jr      NZ, od_cast
        ; clr[k] = clr[k+1]
        add     hl, bc                 ; &clr[k+1]
        ld      a, (hl-)
        ld      (hl), a
        ; hits[k] = hits[k-1] unless the cells differ (C/B clobber the
        ; stride; od_next reloads it)
        ld      hl, #(_r_hits - 2)
        add     hl, de
        add     hl, de                 ; &hits[k-1]
        ld      a, (hl+)
        ld      c, a
        ld      a, (hl+)
        ld      b, a                   ; hits[k-1]
        ld      a, c
        ld      (hl+), a               ; hits[k].lo
        ld      (hl), b                ; hits[k].hi
        inc     hl
        ld      a, (hl+)               ; hits[k+1].lo
        cp      c
        jr      NZ, od_cast
        ld      a, (hl)
        cp      b
        jr      NZ, od_cast
od_next:
        ld      bc, #64
        inc     de
        inc     de
        ld      a, e
        cp      #63
        jr      C, od_loop
        ; column 63 is always cast
        ld      a, #63
        ld      (_r_k), a
        call    ray_cs
        call    ed_cmp                 ; edge between 62 and 63
        jr      od_done
od_cast:
        ld      a, e
        ld      (_r_k), a              ; ray_cs reads r_k
        call    ray_cs                 ; returns DE = k
        call    ed_cmp                 ; edge between k-1 and k
        inc     de
        call    ed_cmp                 ; edge between k and k+1
        dec     de                     ; DE = k again for od_next
        jr      od_next
od_done:

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
        ; next of a column's 4 values at HL: B = max, C = min so far
        .macro  MINMAX ?l1, ?l2
        ld      a, (hl+)
        cp      b
        jr      C, l1
        ld      b, a
        jr      l2
l1:     cp      c
        jr      NC, l2
        ld      c, a
l2:
        .endm

        ; next ray of the column at HL: C = 0 if it is a '=' wall
        .macro  MORT ?ok
        ld      a, (hl+)
        and     #3
        cp      #2
        jr      NZ, ok
        ld      c, #0
ok:
        .endm

fill_go:
        xor     a
        ld      (_r_tc), a
        ldh     (_h_gmx), a
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
        ; tops: maxtop, mintop
        ld      hl, #_r_top
        add     hl, de
        ld      a, (hl+)
        ld      b, a                   ; B = max
        ld      c, a                   ; C = min
        MINMAX
        MINMAX
        MINMAX
        ld      a, b
        ld      (_r_mxt), a
        ldh     a, (_h_gmx)
        cp      b
        jr      NC, fc_gmx
        ld      a, b
        ldh     (_h_gmx), a
fc_gmx:
        ld      a, c
        ldh     (_h_mnt), a
        ; bots: minbot, maxbot
        ld      hl, #_r_bot
        add     hl, de
        ld      a, (hl+)
        ld      b, a
        ld      c, a
        MINMAX
        MINMAX
        MINMAX
        ld      a, c
        ld      (_r_mnb), a
        ld      a, b
        ldh     (_h_mxb), a
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
        ; uniform: no '=' ray (mortar), so every core row is (lo, hi) and
        ; fill leaves those rows to copy_view
        ld      de, #(_r_wt - _r_clr - 4)
        add     hl, de                 ; HL = &r_wt[4*tc] (the packing left r_clr + 4*tc + 4)
        ld      c, #1
        MORT
        MORT
        MORT
        MORT
        ld      a, c
        ld      (r_uni), a
        ; copy_view's descriptor (c_desc + 8*tc, see there): core [maxtop,
        ; minbot) (empty at maxbot if not uniform), lo, hi, min top, max bot
        ld      a, (_r_tc)
        add     a, a
        add     a, a
        add     a, a
        inc     a
        add     a, #<c_desc
        ld      l, a
        ld      a, #>c_desc
        adc     a, #0
        ld      h, a                   ; HL = c_desc + 8*tc + 1
        ldh     a, (_h_mxb)
        dec     c
        jr      Z, 1$
        ld      (hl+), a               ; not uniform: core empty, at maxbot
        ld      (hl+), a
        jr      2$
1$:     ld      a, (_r_mxt)
        ld      (hl+), a
        ld      a, (_r_mnb)
        ld      (hl+), a
2$:     inc     hl                     ; B: copy_view's
        ld      a, (_r_lo)
        ld      (hl+), a
        ld      a, (_r_hi)
        ld      (hl+), a
        ldh     a, (_h_mnt)
        ld      (hl+), a
        ldh     a, (_h_mxb)
        ld      (hl), a

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
        ; A uniform column skips them: copy_view writes them (record R1).
        ld      a, (_r_mxt)
        ld      c, a
        ld      a, (_r_mnb)
        sub     c
        ld      c, a
        ld      a, (r_uni)
        or      a
        jr      Z, 1$
        ld      a, c
        add     a, a
        ld      e, a
        ld      d, #0
        add     hl, de
        jp      fc_end
1$:     ld      a, c
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

        ; project()'s box radius from the farthest wall (cull_r, tables.h)
        ldh     a, (_h_gmx)
        ld      e, a
        ld      d, #0
        ld      hl, #_cull_r
        add     hl, de
        ld      a, (hl)
        ldh     (_pj_r), a
        add     a, a
        ldh     (_pj_r2), a

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
; copy_view: per tile column only rows [t, b): t/b = the union of this
; image's span (min top, max bot of the column's 4 rays, from fill_col's
; c_desc) and the span last copied into this bank (r_vh, set by C's
; view_reset to [0, 64) = copy all). Rows outside a span are pure
; ceiling/floor pattern, the same in every column and image (fill writes
; rows [0,mintop) and [maxbot,64) only from ceil_blk/floor_blk; ragged
; patches and mortar stay inside [top, bot)), so they are already right
; in VRAM. r_vh: [0..15] bank 0x8000 tops, [16..31] bank 0x9000 tops,
; [32..63] the bots. Needs the LCD on.
;
; A column is up to 3 pieces: V [t, core) from vbuf, U = the core rows
; [maxtop, minbot), all (lo, hi), which fill left out of vbuf (no '='
; ray; else the core is empty and vbuf has it), V [core end, b). Phase 1
; (interrupts on) unites the spans and finds the band of lines the shadow
; OAM's sprites cover; cw_next walks the pieces.
; Phase 2 streams the pieces with IME = 0 and IE = STAT (mode-0 request;
; no dispatch at IME = 0). A burst halts until mode 0 starts; mode 0 plus
; the next mode 2 stay open 456 - mode 3 T: >= 167 (mode 3 <= 289 T) on
; sprite lines, >= ~270 elsewhere (SCX = 0, window <= 6 T).
; - V bursts preload 2 bytes, store at ~12, 28, then 20 T a byte; U
;   bursts store a pair per 24 T (~12, 24, 36 ..).
; - sync: poll for mode 2; mode 0 is then >= 172 T away, the halt runs
;   <= ~224 T after mode 2 starts (mode 0 >= 252), burst: 3 V / 5 U pairs.
; - chain: after a burst on line L, the next mode-0 request (line L + 1)
;   is >= 339 T after L's, so arm straight away: sprite lines 3 V / 5 U
;   pairs (halt ~300 T after L's request), others 6 V / 9 U (~228 /
;   ~220 T bursts, halt ~400-430 of >= 443). LY is L or L + 1 here, so
;   the band starts a line early.
; - piece ends take the pairs left (fewer than a burst), then hand the next
;   piece to the chain instead of syncing when the arm (~200 T) fits before
;   the next mode-0 request. Only mode 0 qualifies: the request has passed,
;   so the next one is >= 253 T away (the worst halt bug is ~11 T, and the
;   burst it starts late still fits any mode 0). In mode 2 the request is
;   only >= 172 T away and in mode 3 it is 1..L3 T away (L3 up to 289), so
;   those sync: a handover burst that starts inside the request would need
;   mode 0 to be >= 40 T longer than it can be on a sprite line. Mode 1
;   syncs (its VBlank path). chained pieces are contiguous in VRAM, so the
;   burst only swaps the source.
; The first instruction after each halt is safe to run twice (halt bug),
; so a halt that comes late only starts its burst late; tails start with
; a nop. A chain reaching LY 143 syncs instead, so no IF write lands near
; the VBL request.
; - mode 1 seen while syncing: VBlank. The caller's IE back, ei (the VBL
;   ISR runs now), plain polled pairs as copy_vbuf until VBlank ends.
; Checked cycle by cycle against a PPU model with mode 3 random in
; 172-289 T on sprite lines (scratch tool). The shadow OAM must not
; change during the copy (sprite_update runs after it).
; ---------------------------------------------------------------
LYR  = 0x44                     ; ldh offsets
IFR  = 0x0F
IER  = 0xFF

        .area   _HRAM
_h_seg::        .ds     2       ; cw_next: c_desc of the column
_h_ie::         .ds     1       ; the caller's IE (no STAT: 0x48 has no handler)
_h_nlo::        .ds     1       ; -(first line of the sprite band)
_h_bnm1::       .ds     1       ; its length - 1
_h_typ::        .ds     1       ; current record: 0 V, else U
_h_bank::       .ds     1       ; r_dst hi: 0x80 or 0x90
_h_cnt::        .ds     1       ; phase 1: columns left
_h_stg::        .ds     1       ; cw_next: piece of the column
_h_ctc::        .ds     1       ;   and the column
        .area   _CODE

        .macro  BYTE
        ld      a, (hl+)
        ld      (de), a
        inc     e                      ; never wraps inside a 128-byte column
        .endm
        ; V: preload A, B, halt until mode 0, store them, then n BYTEs
        .macro  BURST n
        ld      a, (hl+)
        ld      b, (hl)
        inc     hl
        halt
        ld      (de), a
        inc     e
        ld      a, b
        ld      (de), a
        inc     e
        .rept   n
        BYTE
        .endm
        .endm
        ; U: one (lo, hi) pair at HL
        .macro  UPAIR
        ld      (hl), c
        inc     l                      ; never wraps inside a 128-byte column
        ld      (hl), b
        inc     l
        .endm
        ; U record end: n pairs
        .macro  UTAIL n
        halt
        nop
        .rept   n
        UPAIR
        .endm
        jp      cw_segend
        .endm

_copy_view::
        ; ---- this image's span per column united with the bank's old one
        ; (r_vh) -> descriptor T, B (c_desc + 0, + 3)
        ld      a, (_r_dst+1)
        ldh     (_h_bank), a
        and     #0x10                  ; 0x80 -> 0, 0x90 -> 16
        add     a, #<_r_vh
        ld      c, a
        ld      a, #>_r_vh
        adc     a, #0
        ld      b, a                   ; BC = this bank's old tops (bots 32 on)
        ld      hl, #(c_desc + 6)
        ld      a, #16
        ldh     (_h_cnt), a
cw_col:
        ld      a, (hl+)               ; min top
        ld      d, a
        ld      a, (bc)
        ld      e, a                   ; old top
        ld      a, d
        ld      (bc), a
        cp      e
        jr      C, 1$
        ld      d, e                   ; D = T
1$:     ld      a, (hl)                ; max bot
        push    hl
        ld      hl, #32
        add     hl, bc
        ld      e, (hl)                ; old bot
        ld      (hl), a
        cp      e
        jr      NC, 2$
        ld      a, e                   ; A = B
2$:     pop     hl
        ld      e, a
        ld      a, l
        sub     #7
        ld      l, a
        jr      NC, 3$
        dec     h                      ; HL = this column's T
3$:     ld      (hl), d
        inc     hl
        inc     hl
        inc     hl
        ld      (hl), e                ; B
        ld      de, #11
        add     hl, de                 ; next column's min top
        inc     bc
        ldh     a, (_h_cnt)
        dec     a
        ldh     (_h_cnt), a
        jr      NZ, cw_col

        ; ---- sprite band: lines [min y - 16, max y - 1] of the 8x16 OBJ
        ; with OAM y in 1..159 (0 = hidden); none: line 200, so no LY hits
        ld      hl, #_shadow_OAM
        ld      bc, #0xFF00            ; B = min y, C = max y
        ld      e, #40
21$:    ld      a, (hl)
        dec     a
        cp      #159
        jr      NC, 22$                ; y = 0 or >= 160: off screen
        inc     a
        cp      b
        jr      NC, 23$
        ld      b, a
23$:    cp      c
        jr      C, 22$
        ld      c, a
22$:    inc     hl
        inc     hl
        inc     hl
        inc     hl
        dec     e
        jr      NZ, 21$
        ld      a, c
        sub     b
        jr      C, 24$                 ; no sprite
        add     a, #16                 ; length - 1: max y - min y + 16 lines,
        ldh     (_h_bnm1), a           ;   from a line early (chain's LY)
        ld      a, #17
        sub     b
        jr      25$
24$:    xor     a                      ; none: band = line 200 alone
        ldh     (_h_bnm1), a
        ld      a, #56
25$:    ldh     (_h_nlo), a            ; -(min y - 17)

        ; ---- phase 2
        ld      a, #<c_desc
        ldh     (_h_seg), a
        ld      a, #>c_desc
        ldh     (_h_seg+1), a
        xor     a
        ldh     (_h_stg), a
        ldh     (_h_ctc), a
        ldh     a, (IER)
        ldh     (_h_ie), a
        di
        ld      a, #0x08
        ldh     (STATR), a             ; STAT request on mode 0
        ld      a, #0x02
        ldh     (IER), a               ; only it ends a halt
        call    cw_next                ; there is always one
cw_sync:                               ; IME = 0: poll for mode 2 (or 1)
        ldh     a, (STATR)
        and     #3
        cp      #2
        jr      Z, cw_s2
        dec     a
        jr      NZ, cw_sync            ; mode 0 or 3
        ; ---- mode 1: VBlank
        ldh     a, (_h_ie)
        ldh     (IER), a
        ei
        nop                            ; the VBL ISR runs here, before a poll
cv_go:  ldh     a, (_h_typ)
        or      a
        jr      NZ, cu_vpoll
cv_poll:                               ; V record
        ldh     a, (STATR)
        and     #3
        jr      Z, cv_pair
        dec     a
        jr      Z, cv_pair             ; still VBlank
cv_out: di                             ; mode 2/3 of line 0: back to halts
        ld      a, #0x02
        ldh     (IER), a
        jr      cw_sync
cv_pair:
        ld      a, (hl+)
        ld      (de), a
        inc     e
        ld      a, (hl+)
        ld      (de), a
        inc     e
        dec     c
        jr      NZ, cv_poll
cv_next:
        call    cw_next
        jr      NZ, cv_go
        jp      cw_end
cu_vpoll:                              ; U record
        ldh     a, (STATR)
        and     #3
        jr      Z, cu_vpair
        dec     a
        jr      NZ, cv_out
cu_vpair:
        UPAIR
        dec     d
        jr      NZ, cu_vpoll
        jr      cv_next

cw_s2:                                 ; mode 2: mode 0 >= 172 T away
        ldh     a, (IFR)
        and     #0xFD                  ; the last line's mode-0 request
        ldh     (IFR), a
        ldh     a, (_h_typ)
        or      a
        jr      Z, cw_3
        ld      a, d
        jp      cu_5
cw_chain:                              ; right after a V burst on line L
        ldh     a, (LYR)
        cp      #143
        jr      NC, cw_sync            ; VBlank next
        ld      b, a
        ldh     a, (IFR)
        and     #0xFD
        ldh     (IFR), a
        ldh     a, (_h_nlo)
        add     a, b
        ld      b, a                   ; LY - band start
        ldh     a, (_h_bnm1)
        cp      b                      ; carry: outside the sprite band
        jr      C, cw_6
cw_3:   ld      a, c                   ; C = pairs left
        cp      #3
        jp      C, cw_tail
        sub     #3
        jr      Z, cw_3l
        ld      c, a
        BURST   4                      ; 3 pairs
        jp      cw_chain
cw_3l:  BURST   4
        jp      cw_segend
cw_6:   ld      a, c
        cp      #6
        jr      C, cw_tail
        sub     #6
        jr      Z, cw_6l
        ld      c, a
        BURST   10                     ; 6 pairs
        jp      cw_chain
cw_6l:  BURST   10
        jp      cw_segend
cw_tail:                               ; 1-5 pairs, fewer than the burst (<= 2 on sprite lines)
        dec     a
        jr      Z, 1$
        dec     a
        jr      Z, 2$
        dec     a
        jr      Z, 3$
        dec     a
        jr      Z, 4$
        halt                           ; 5 (outside the band only)
        nop
        .rept   10
        BYTE
        .endm
        jp      cw_segend
4$:     halt                           ; 4 (outside the band only)
        nop
        .rept   8
        BYTE
        .endm
        jp      cw_segend
3$:     halt
        nop
        .rept   6
        BYTE
        .endm
        jr      cw_segend
2$:     halt
        nop
        .rept   4
        BYTE
        .endm
        jr      cw_segend
1$:     halt
        nop
        BYTE
        BYTE
cw_segend:                             ; piece end: hand the next piece's
        call    cw_next                ; first burst to the chain when the
        jr      Z, cw_end              ; next mode-0 request is far enough for
        ldh     a, (STATR)             ; its arm (~200 T), instead of
        and     #3                     ; polling for mode 2; the pieces are
        jr      Z, 1$                  ; contiguous in VRAM, so the burst just
        jp      cw_sync                ; swaps the source
1$:     ldh     a, (_h_typ)            ; mode 0: the request has passed, the
        or      a                      ;   next is >= 253 T away
        jr      NZ, cu_chain
        jp      cw_chain
cw_end:
        xor     a
        ldh     (STATR), a
        ldh     a, (_h_ie)
        ldh     (IER), a
        ei
        ret

; U records: HL = dst, C = lo, B = hi, D = pairs left
cu_chain:                              ; right after a U burst on line L
        ldh     a, (LYR)
        cp      #143
        jp      NC, cw_sync            ; VBlank next
        ld      e, a
        ldh     a, (IFR)
        and     #0xFD
        ldh     (IFR), a
        ldh     a, (_h_nlo)
        add     a, e
        ld      e, a                   ; LY - band start
        ldh     a, (_h_bnm1)
        cp      e                      ; carry: outside the sprite band
        ld      a, d
        jr      C, cu_9
cu_5:   cp      #5                     ; A = pairs left
        jp      C, cu_tail
        sub     #5
        ld      d, a
        jr      Z, cu_5l
        halt
        .rept   5
        UPAIR
        .endm
        jp      cu_chain
cu_5l:  halt
        .rept   5
        UPAIR
        .endm
        jp      cw_segend
cu_9:   cp      #9
        jp      C, cu_tail
        sub     #9
        ld      d, a
        jr      Z, cu_9l
        halt
        .rept   9
        UPAIR
        .endm
        jp      cu_chain
cu_9l:  halt
        .rept   9
        UPAIR
        .endm
        jp      cw_segend
cu_tail:                               ; 1-8 pairs, fewer than the burst (<= 4 on sprite lines)
        dec     a
        jr      Z, cu_t1
        dec     a
        jr      Z, cu_t2
        dec     a
        jr      Z, cu_t3
        dec     a
        jr      Z, cu_t4
        dec     a
        jr      Z, cu_t5
        dec     a
        jp      Z, cu_t6
        dec     a
        jp      Z, cu_t7
        UTAIL   8
cu_t1:  UTAIL   1
cu_t2:  UTAIL   2
cu_t3:  UTAIL   3
cu_t4:  UTAIL   4
cu_t5:  UTAIL   5
cu_t6:  UTAIL   6
cu_t7:  UTAIL   7

; next non-empty piece -> NZ: V: C = pairs, HL = src, DE = dst; U: D =
; pairs, HL = dst, C = lo, B = hi (h_typ says which). Z after the last
; column. Piece h_stg (0 V, 1 U, 2 V) of column h_ctc runs between the
; c_desc boundaries at h_seg and h_seg + 1.
cw_next:
        ldh     a, (_h_seg)
        ld      l, a
        ldh     a, (_h_seg+1)
        ld      h, a
cn_loop:
        ldh     a, (_h_stg)
        cp      #3
        jr      NZ, 1$
        ld      de, #5                 ; B -> the next column's T
        add     hl, de
        ldh     a, (_h_ctc)
        inc     a
        cp      #16
        ret     Z                      ; done
        ldh     (_h_ctc), a
        xor     a
1$:     ld      d, a                   ; D = piece
        inc     a
        ldh     (_h_stg), a
        ld      a, (hl+)               ; first row
        ld      e, a
        ld      a, (hl)                ; end row
        sub     e
        jr      Z, cn_loop             ; empty
        ld      c, a                   ; pairs
        ld      a, l
        ldh     (_h_seg), a
        ld      a, h
        ldh     (_h_seg+1), a          ; the next piece starts here
        ldh     a, (_h_ctc)            ; offset = (col & 1) << 7 | 2*row, col >> 1
        rrca
        ld      b, a
        and     #0x80
        sla     e
        add     a, e
        ld      e, a
        ld      a, b
        and     #0x7F
        ld      b, a
        dec     d
        jr      Z, cn_u
        ld      a, e                   ; V: src = vbuf + offset, dst = bank + offset
        add     a, #<_vbuf
        ld      l, a
        ld      a, b
        adc     a, #>_vbuf
        ld      h, a
        ldh     a, (_h_bank)
        add     a, b
        ld      d, a
        xor     a
        ldh     (_h_typ), a
        or      #1                     ; NZ
        ret
cn_u:   ld      d, c                   ; U: pairs
        inc     hl
        inc     hl
        ld      c, (hl)                ; lo (HL was at the core end)
        inc     hl
        ld      a, (hl)                ; hi
        push    af
        ldh     a, (_h_bank)
        add     a, b
        ld      h, a
        ld      l, e                   ; HL = dst
        pop     af
        ld      b, a
        ld      a, d
        ldh     (_h_typ), a            ; nonzero
        or      a                      ; NZ
        ret

; ---------------------------------------------------------------
; check_view (make CHECK=1, hardware test): compare the bank copy_view
; just wrote (r_dst) with this image: the uniform cores (c_desc) as
; (lo, hi), vbuf everywhere else. Reads poll STAT like copy_vbuf. ->
; A = mismatching row pairs (255 at most).
; ---------------------------------------------------------------
        .area   _HRAM
_h_cn::         .ds     1       ; check_view: core rows of the column
_h_clo::        .ds     1
_h_chi::        .ds     1
_h_e0::         .ds     1       ; expected pair (e1 must follow e0)
_h_e1::         .ds     1
_h_r0::         .ds     1
_h_cerr::       .ds     1
        .area   _CODE
_check_view::
        xor     a
        ldh     (_h_cerr), a
        ldh     (_h_ctc), a
        ld      a, #<c_desc
        ldh     (_h_seg), a
        ld      a, #>c_desc
        ldh     (_h_seg+1), a
ck_col:
        ldh     a, (_h_seg)
        ld      l, a
        ldh     a, (_h_seg+1)
        ld      h, a
        inc     hl
        ld      a, (hl+)               ; core start
        ld      c, a
        ld      a, (hl+)               ; core end
        sub     c
        ldh     (_h_cn), a             ; core rows (0: none)
        inc     hl
        ld      a, (hl+)
        ldh     (_h_clo), a
        ld      a, (hl+)
        ldh     (_h_chi), a
        inc     hl
        inc     hl                     ; the next column's c_desc
        ld      a, l
        ldh     (_h_seg), a
        ld      a, h
        ldh     (_h_seg+1), a
        ldh     a, (_h_ctc)            ; offset = (col & 1) << 7, col >> 1
        rrca
        ld      d, a
        and     #0x80
        ld      e, a
        ld      a, d
        and     #0x7F
        ld      d, a
        ld      hl, #_vbuf
        add     hl, de                 ; HL = src
        ld      a, (_r_dst+1)
        add     a, d
        ld      d, a                   ; DE = dst
        ld      b, #0                  ; row
ck_row:
        ld      a, b
        sub     c                      ; row - core start
        push    hl
        ld      hl, #_h_cn
        cp      (hl)
        pop     hl
        jr      NC, 1$
        ldh     a, (_h_clo)            ; a core row
        ldh     (_h_e0), a
        ldh     a, (_h_chi)
        ldh     (_h_e1), a
        jr      2$
1$:     ld      a, (hl+)
        ldh     (_h_e0), a
        ld      a, (hl-)
        ldh     (_h_e1), a
2$:     inc     hl
        inc     hl
3$:     ldh     a, (STATR)             ; mode 0/1: >= 80 T open
        and     #0x02
        jr      NZ, 3$
        ld      a, (de)
        ldh     (_h_r0), a
        inc     e
        ld      a, (de)
        inc     de
        push    hl
        ld      hl, #_h_e1
        cp      (hl)
        jr      NZ, 4$
        ldh     a, (_h_r0)
        dec     l                      ; _h_e0
        cp      (hl)
        jr      Z, 5$
4$:     ld      hl, #_h_cerr
        inc     (hl)
        jr      NZ, 5$
        dec     (hl)                   ; stays at 255
5$:     pop     hl
        inc     b
        bit     6, b
        jr      Z, ck_row
        ldh     a, (_h_ctc)
        inc     a
        ldh     (_h_ctc), a
        cp      #16
        jp      NZ, ck_col
        ldh     a, (_h_cerr)
        ret
