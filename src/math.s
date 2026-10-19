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
