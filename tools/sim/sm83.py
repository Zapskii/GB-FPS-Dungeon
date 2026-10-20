"""Minimal SM83 interpreter with exact cycle counts (no PPU, no interrupts).
run(mem, pc, sp) executes until the outermost ret; returns (cycles, per-pc Counter)."""
import collections

R8 = ['b', 'c', 'd', 'e', 'h', 'l', None, 'a']


class CPU:
    def __init__(self, mem):
        self.m = mem
        self.a = self.b = self.c = self.d = self.e = self.h = self.l = 0
        self.fz = self.fn = self.fh = self.fc = 0
        self.sp = 0xDFF0
        self.pc = 0

    # register helpers
    def hl(self): return self.h << 8 | self.l
    def sethl(self, v): self.h = v >> 8 & 0xFF; self.l = v & 0xFF
    def rp(self, i):
        if i == 0: return self.b << 8 | self.c
        if i == 1: return self.d << 8 | self.e
        if i == 2: return self.h << 8 | self.l
        return self.sp
    def setrp(self, i, v):
        v &= 0xFFFF
        if i == 0: self.b, self.c = v >> 8, v & 0xFF
        elif i == 1: self.d, self.e = v >> 8, v & 0xFF
        elif i == 2: self.h, self.l = v >> 8, v & 0xFF
        else: self.sp = v
    def r(self, i):
        return self.m[self.hl()] if i == 6 else getattr(self, R8[i])
    def setr(self, i, v):
        if i == 6: self.m[self.hl()] = v & 0xFF
        else: setattr(self, R8[i], v & 0xFF)
    def f(self): return self.fz << 7 | self.fn << 6 | self.fh << 5 | self.fc << 4
    def setf(self, v): self.fz, self.fn, self.fh, self.fc = v >> 7 & 1, v >> 6 & 1, v >> 5 & 1, v >> 4 & 1
    def rd8(self):
        v = self.m[self.pc]; self.pc = (self.pc + 1) & 0xFFFF; return v
    def rd16(self):
        v = self.m[self.pc] | self.m[(self.pc + 1) & 0xFFFF] << 8; self.pc = (self.pc + 2) & 0xFFFF; return v
    def push(self, v):
        self.sp = (self.sp - 1) & 0xFFFF; self.m[self.sp] = v >> 8 & 0xFF
        self.sp = (self.sp - 1) & 0xFFFF; self.m[self.sp] = v & 0xFF
    def pop(self):
        v = self.m[self.sp] | self.m[(self.sp + 1) & 0xFFFF] << 8; self.sp = (self.sp + 2) & 0xFFFF; return v
    def cond(self, cc):
        return (not self.fz, self.fz, not self.fc, self.fc)[cc]

    def alu(self, op, v):
        a = self.a
        if op == 0 or op == 1:          # add, adc
            c = self.fc if op == 1 else 0
            r = a + v + c
            self.fh = ((a & 15) + (v & 15) + c) > 15; self.fc = r > 255; self.fn = 0
            self.a = r & 0xFF; self.fz = self.a == 0
        elif op in (2, 3, 7):           # sub, sbc, cp
            c = self.fc if op == 3 else 0
            r = a - v - c
            self.fh = ((a & 15) - (v & 15) - c) < 0; self.fc = r < 0; self.fn = 1
            self.fz = (r & 0xFF) == 0
            if op != 7: self.a = r & 0xFF
        elif op == 4:
            self.a = a & v; self.fz = self.a == 0; self.fn = 0; self.fh = 1; self.fc = 0
        elif op == 5:
            self.a = a ^ v; self.fz = self.a == 0; self.fn = self.fh = self.fc = 0
        elif op == 6:
            self.a = a | v; self.fz = self.a == 0; self.fn = self.fh = self.fc = 0
        self.fz, self.fh, self.fc = int(bool(self.fz)), int(bool(self.fh)), int(bool(self.fc))

    def cb(self):
        op = self.rd8()
        x, y, z = op >> 6, op >> 3 & 7, op & 7
        v = self.r(z)
        cyc = 16 if z == 6 else 8
        if x == 0:
            c = self.fc
            if y == 0: r = (v << 1 | v >> 7) & 0xFF; nc = v >> 7
            elif y == 1: r = (v >> 1 | v << 7) & 0xFF; nc = v & 1
            elif y == 2: r = (v << 1 | c) & 0xFF; nc = v >> 7
            elif y == 3: r = (v >> 1 | c << 7) & 0xFF; nc = v & 1
            elif y == 4: r = (v << 1) & 0xFF; nc = v >> 7
            elif y == 5: r = (v >> 1 | (v & 0x80)) & 0xFF; nc = v & 1
            elif y == 6: r = (v >> 4 | v << 4) & 0xFF; nc = 0
            else: r = v >> 1; nc = v & 1
            self.setr(z, r); self.fz = int(r == 0); self.fn = self.fh = 0; self.fc = nc
        elif x == 1:
            self.fz = int(not (v >> y & 1)); self.fn = 0; self.fh = 1
            cyc = 12 if z == 6 else 8
        elif x == 2: self.setr(z, v & ~(1 << y))
        else: self.setr(z, v | 1 << y)
        return cyc

    def step(self):
        op = self.rd8()
        x, y, z, p, q = op >> 6, op >> 3 & 7, op & 7, op >> 4 & 3, op >> 3 & 1
        if x == 1:
            if op == 0x76: raise RuntimeError('halt')
            self.setr(y, self.r(z)); return 8 if (y == 6 or z == 6) else 4
        if x == 2:
            self.alu(y, self.r(z)); return 8 if z == 6 else 4
        if x == 0:
            if z == 0:
                if y == 0: return 4
                if y == 1:
                    a = self.rd16(); self.m[a] = self.sp & 0xFF; self.m[a + 1] = self.sp >> 8; return 20
                if y == 3:
                    d = self.rd8(); self.pc = (self.pc + (d - 256 if d > 127 else d)) & 0xFFFF; return 12
                if y >= 4:
                    d = self.rd8()
                    if self.cond(y - 4):
                        self.pc = (self.pc + (d - 256 if d > 127 else d)) & 0xFFFF; return 12
                    return 8
                raise RuntimeError('stop')
            if z == 1:
                if q == 0: self.setrp(p, self.rd16()); return 12
                hl, v = self.hl(), self.rp(p)
                r = hl + v
                self.fn = 0; self.fh = int((hl & 0xFFF) + (v & 0xFFF) > 0xFFF); self.fc = int(r > 0xFFFF)
                self.sethl(r & 0xFFFF); return 8
            if z == 2:
                addr = (self.rp(0), self.rp(1), self.hl(), self.hl())[p]
                if q == 0: self.m[addr] = self.a
                else: self.a = self.m[addr]
                if p == 2: self.sethl((addr + 1) & 0xFFFF)
                if p == 3: self.sethl((addr - 1) & 0xFFFF)
                return 8
            if z == 3:
                self.setrp(p, self.rp(p) + (1 if q == 0 else -1)); return 8
            if z == 4 or z == 5:
                v = self.r(y)
                if z == 4: r = (v + 1) & 0xFF; self.fh = int((v & 15) == 15); self.fn = 0
                else: r = (v - 1) & 0xFF; self.fh = int((v & 15) == 0); self.fn = 1
                self.setr(y, r); self.fz = int(r == 0); return 12 if y == 6 else 4
            if z == 6:
                self.setr(y, self.rd8()); return 12 if y == 6 else 8
            # z == 7
            a, c = self.a, self.fc
            if y == 0: self.a = (a << 1 | a >> 7) & 0xFF; self.fc = a >> 7
            elif y == 1: self.a = (a >> 1 | a << 7) & 0xFF; self.fc = a & 1
            elif y == 2: self.a = (a << 1 | c) & 0xFF; self.fc = a >> 7
            elif y == 3: self.a = (a >> 1 | c << 7) & 0xFF; self.fc = a & 1
            elif y == 4: raise RuntimeError('daa')
            elif y == 5: self.a = a ^ 0xFF; self.fn = self.fh = 1; return 4
            elif y == 6: self.fc = 1; self.fn = self.fh = 0; return 4
            else: self.fc ^= 1; self.fn = self.fh = 0; return 4
            self.fz = self.fn = self.fh = 0; return 4
        # x == 3
        if z == 0:
            if y < 4:
                if self.cond(y): self.pc = self.pop(); return 20
                return 8
            if y == 4: self.m[0xFF00 + self.rd8()] = self.a; return 12
            if y == 6: self.a = self.m[0xFF00 + self.rd8()]; return 12
            d = self.rd8()
            sd = d - 256 if d > 127 else d
            r = (self.sp + sd) & 0xFFFF
            self.fz = self.fn = 0
            self.fh = int((self.sp & 0xF) + (d & 0xF) > 0xF)
            self.fc = int((self.sp & 0xFF) + d > 0xFF)
            if y == 5: self.sp = r; return 16
            self.sethl(r); return 12
        if z == 1:
            if q == 0:
                v = self.pop()
                if p == 3: self.a = v >> 8; self.setf(v & 0xF0)
                else: self.setrp(p, v)
                return 12
            if p == 0: self.pc = self.pop(); return 16
            if p == 1: self.pc = self.pop(); return 16          # reti
            if p == 2: self.pc = self.hl(); return 4
            self.sp = self.hl(); return 8
        if z == 2:
            if y < 4:
                a = self.rd16()
                if self.cond(y): self.pc = a; return 16
                return 12
            if y == 4: self.m[0xFF00 + self.c] = self.a; return 8
            if y == 6: self.a = self.m[0xFF00 + self.c]; return 8
            if y == 5: self.m[self.rd16()] = self.a; return 16
            self.a = self.m[self.rd16()]; return 16
        if z == 3:
            if y == 0: self.pc = self.rd16(); return 16
            if y == 1: return self.cb()
            if y in (6, 7): return 4                           # di / ei
            raise RuntimeError('bad op %02X' % op)
        if z == 4:
            a = self.rd16()
            if y < 4 and self.cond(y): self.push(self.pc); self.pc = a; return 24
            if y < 4: return 12
            raise RuntimeError('bad op %02X' % op)
        if z == 5:
            if q == 0:
                v = (self.b << 8 | self.c, self.d << 8 | self.e, self.hl(), self.a << 8 | self.f())[p]
                self.push(v); return 16
            if p == 0:
                a = self.rd16(); self.push(self.pc); self.pc = a; return 24
            raise RuntimeError('bad op %02X' % op)
        if z == 6:
            self.alu(y, self.rd8()); return 8
        self.push(self.pc); self.pc = y * 8; return 16


def run(mem, pc, regs=None, limit=5_000_000):
    cpu = CPU(mem)
    cpu.pc = pc
    if regs:
        for k, v in regs.items(): setattr(cpu, k, v)
    sp0 = cpu.sp
    cpu.push(0xFFFF)
    prof = collections.Counter()
    total = 0
    while cpu.pc != 0xFFFF:
        at = cpu.pc
        c = cpu.step()
        prof[at] += c
        total += c
        if total > limit: raise RuntimeError('runaway at %04X' % at)
    return total, prof, cpu
