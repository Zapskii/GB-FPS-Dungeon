; Small fixed-range arithmetic for project() (src/main.c), replacing SDCC's
; __divuint (~2.5k T) and __mulint (~330-800 T). sdcccall(1): first 16-bit
; arg in DE, 8-bit arg in A; 8-bit result in A, 16-bit result in BC.

        .module math
        .area   _CODE

; uint8_t udiv8(uint16_t n, uint8_t d): n / d, needs (n >> 8) < d so the
; quotient fits a byte. Restoring shift-subtract, ~450 T.
_udiv8::
        ld      c,a             ; C = divisor
        ld      a,d             ; A = remainder (high byte), E = low byte
        ld      b,#8
1$:     sla     e               ; next dividend bit into carry, quotient bit 0 = 0
        rla                     ; rem = rem*2 + bit
        jr      c,2$            ; 9-bit rem: certainly >= d
        cp      c
        jr      c,3$
2$:     sub     c
        inc     e               ; quotient bit = 1
3$:     dec     b
        jr      nz,1$
        ld      a,e
        ret

; int16_t smul8(int16_t r, int8_t c): r * c, needs |r| <= 255, |c| <= 127.
; Unrolled 7-bit shift-add on magnitudes, ~250 T.
_smul8::
        ld      l,a             ; L = c
        xor     d
        ld      c,a             ; C bit 7 = sign of the product
        bit     7,d
        jr      z,1$
        xor     a
        sub     e
        ld      e,a             ; E = |r|
1$:     ld      a,l
        bit     7,a
        jr      z,2$
        cpl
        inc     a               ; A = |c|
2$:     ld      hl,#0
        ld      d,h
        add     a,a             ; drop bit 7 (0)
        add     a,a
        jr      nc,3$
        add     hl,de
3$:     add     hl,hl
        add     a,a
        jr      nc,4$
        add     hl,de
4$:     add     hl,hl
        add     a,a
        jr      nc,5$
        add     hl,de
5$:     add     hl,hl
        add     a,a
        jr      nc,6$
        add     hl,de
6$:     add     hl,hl
        add     a,a
        jr      nc,7$
        add     hl,de
7$:     add     hl,hl
        add     a,a
        jr      nc,8$
        add     hl,de
8$:     add     hl,hl
        add     a,a
        jr      nc,9$
        add     hl,de
9$:     bit     7,c
        jr      nz,10$
        ld      b,h
        ld      c,l
        ret
10$:    xor     a
        sub     l
        ld      c,a
        ld      a,#0
        sbc     a,h
        ld      b,a
        ret

; uint8_t project(uint16_t wx, uint16_t wy): a world point (8.8) through
; the camera, as main.c documents it: 1 when it is within 10 cells on both
; axes, in front (fwd 96..2560), inside the 90 deg FOV, at most 60 px off
; centre and nearer than the wall at its column (r_zb). Sets p_fwd (always,
; once computed), p_x, p_foot. DE = wx, BC = wy.
        .area   _HRAM
pj_cs:  .ds     1
pj_sn:  .ds     1
        .area   _CODE
_project::
        ld      hl, #_cam_x            ; DE = rx >> 4
        ld      a, e
        sub     (hl)
        ld      e, a
        inc     hl
        ld      a, d
        sbc     a, (hl)
        ld      d, a
        call    pj_q
        jp      C, pj_no
        push    de                     ; qx
        ld      hl, #_cam_y            ; DE = ry >> 4
        ld      a, c
        sub     (hl)
        ld      e, a
        inc     hl
        ld      a, b
        sbc     a, (hl)
        ld      d, a
        call    pj_q
        jp      C, pj_no1
        push    de                     ; qy; stack: qy, qx
        ld      a, (_cam_a)
        ld      e, a
        ld      d, #0
        ld      hl, #_cos8
        add     hl, de
        ld      a, (hl)
        ldh     (pj_cs), a
        ld      hl, #_sin8
        add     hl, de
        ld      a, (hl)
        ldh     (pj_sn), a
        ; fwd = (qx*cs + qy*sn) >> 3
        pop     de
        push    de
        call    _smul8                 ; BC = qy*sn
        push    bc
        ld      hl, #4
        add     hl, sp
        ld      a, (hl+)
        ld      e, a
        ld      d, (hl)                ; qx
        ldh     a, (pj_cs)
        call    _smul8
        pop     hl
        add     hl, bc
        call    pj_sr3
        ld      a, l
        ld      (_p_fwd), a
        ld      a, h
        ld      (_p_fwd+1), a
        bit     7, h
        jp      NZ, pj_no2             ; behind
        or      a
        jr      NZ, 1$
        ld      a, l
        cp      #96
        jp      C, pj_no2              ; too near
        jr      2$
1$:     cp      #10                    ; > 2560: too far
        jr      C, 2$
        jp      NZ, pj_no2
        ld      a, l
        or      a
        jp      NZ, pj_no2
2$:     ; lat = (qy*cs - qx*sn) >> 3
        ld      hl, #2
        add     hl, sp
        ld      a, (hl+)
        ld      e, a
        ld      d, (hl)                ; qx
        ldh     a, (pj_sn)
        call    _smul8
        pop     de                     ; qy
        push    bc                     ; qx*sn; stack: qx*sn, qx
        ldh     a, (pj_cs)
        call    _smul8
        pop     hl
        ld      a, c                   ; HL = qy*cs - qx*sn
        sub     l
        ld      l, a
        ld      a, b
        sbc     a, h
        ld      h, a
        call    pj_sr3
        pop     de                     ; drop qx
        ld      c, h                   ; C bit 7 = lat < 0
        bit     7, h
        jr      Z, 3$
        xor     a
        sub     l
        ld      l, a
        ld      a, #0
        sbc     a, h
        ld      h, a                   ; HL = |lat|
3$:     ld      a, (_p_fwd)            ; |lat| >= fwd: outside the FOV
        ld      e, a
        ld      a, (_p_fwd+1)
        ld      d, a
        ld      a, l
        sub     e
        ld      a, h
        sbc     a, d
        jp      NC, pj_no
        ; off = udiv8(|lat| << 2, fwd >> 4) (= 64 lat / fwd)
        ld      a, e                   ; DE = fwd >> 4 (<= 160)
        srl     d
        rra
        srl     d
        rra
        srl     d
        rra
        srl     d
        rra
        add     hl, hl
        add     hl, hl
        ld      d, h
        ld      e, l
        push    bc
        call    _udiv8
        pop     bc
        cp      #61
        jp      NC, pj_no
        bit     7, c
        jr      Z, 4$
        cpl
        inc     a
4$:     add     a, #80
        ld      (_p_x), a
        sub     #16                    ; VIEW_X0
        srl     a
        ld      e, a
        ld      d, #0
        ld      hl, #_r_zb
        add     hl, de
        ld      b, (hl)                ; the wall at its column, 1/8 cells
        ld      a, (_p_fwd)            ; k = fwd >> 5 (<= 80)
        ld      e, a
        ld      a, (_p_fwd+1)
        ld      d, a
        ld      a, e
        srl     d
        rra
        srl     d
        rra
        srl     d
        rra
        srl     d
        rra
        srl     d
        rra
        cp      b
        jr      Z, 5$
        jp      NC, pj_no              ; behind the wall
5$:     ld      e, a
        ld      d, #0
        ld      hl, #_foot_tab
        add     hl, de
        ld      a, (hl)
        ld      (_p_foot), a
        ld      a, #1
        ret
pj_no2: pop     de
pj_no1: pop     de
pj_no:  xor     a
        ret

; DE = r (|r| <= 2560 on both sides, else carry) -> DE = r >> 4 (arith)
pj_q:   ld      a, d
        add     a, #10
        cp      #20
        jr      C, 1$
        jr      NZ, 2$                 ; hi byte 10 only with lo 0 (2560)
        ld      a, e
        or      a
        jr      NZ, 2$
1$:     sra     d
        rr      e
        sra     d
        rr      e
        sra     d
        rr      e
        sra     d
        rr      e
        or      a                      ; no carry
        ret
2$:     scf
        ret

; HL >>= 3 (arithmetic)
pj_sr3: sra     h
        rr      l
        sra     h
        rr      l
        sra     h
        rr      l
        ret
