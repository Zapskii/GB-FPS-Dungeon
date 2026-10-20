"""Cycle-level check of copy_view: SM83 interpreter + DMG PPU/interrupt model.
Line = 456 T: mode 2 [0,80), mode 3 [80, 80+L3), mode 0 rest (lines 0-143),
mode 1 on 144-153. IF.1 rises at mode-0 start (STAT bit 3), IF.0 at line 144.
VRAM writes in mode 3 are violations; slack = T from a write to the next
mode-3 start. The VBL ISR is a stand-in that burns isr_t T."""
import random
import sm83

ACC_AT = {0xF0: 8, 0xE0: 8, 0xF2: 4, 0xE2: 4, 0xFA: 12, 0xEA: 12, 0x36: 8,
          0x34: 8, 0x35: 8, 0xCB: 12}


class Mem:
    def __init__(self, raw, sim):
        self.raw, self.sim = raw, sim

    def __getitem__(self, a):
        if a == 0xFF41: return self.sim.stat()
        if a == 0xFF44: return self.sim.ly()
        if a == 0xFF0F: return self.sim.iflag | 0xE0
        return self.raw[a]

    def __setitem__(self, a, v):
        s = self.sim
        if 0x8000 <= a < 0xA000:
            s.vram_write(a, v)
        elif a == 0xFF0F: s.iflag = v & 0x1F
        elif a == 0xFF41: s.stat_en = v & 0x78
        elif a == 0xFFFF: s.ie = v & 0x1F
        self.raw[a] = v & 0xFF


class Sim:
    def __init__(self, raw, l3, t0=0, isr_t=3000):
        self.l3 = l3                      # per line mode-3 length (0..143)
        self.t = t0
        self.iflag = 0
        self.stat_en = 0
        self.ie = raw[0xFFFF]
        self.ime = 1
        self.isr_t = isr_t
        self.viol = []
        self.slack = 10 ** 9
        self.slack_at = None
        self.late = []                    # (T late, pc) of halts that found a request
        self.if1_t = 0
        self.writes = 0
        self.mem = Mem(raw, self)
        self.raw = raw
        self.cpu = sm83.CPU(self.mem)
        self.access_t = 0
        self.cpu_pc = 0
        import collections
        self.pcs = collections.Counter()
        self.pcc = collections.Counter()      # T per pc (halts not included)
        self.pcslack = {}
        self.last = self.t                # for edge detection

    # PPU state at absolute time t
    def at(self, t):
        f = t % (154 * 456)
        y, x = divmod(f, 456)
        if y >= 144: return y, 1, x
        if x < 80: return y, 2, x
        if x < 80 + self.l3[y]: return y, 3, x
        return y, 0, x

    def stat(self):
        y, m, x = self.at(self.access_t)
        return 0x80 | self.stat_en | m

    def ly(self):
        return self.at(self.access_t)[0]

    def vram_write(self, a, v):
        y, m, x = self.at(self.access_t)
        self.writes += 1
        if m == 3:
            self.viol.append((self.access_t, y, x, self.cpu_pc, self.l3[y] if y < 144 else 0, self.l3[y - 1] if 0 < y < 145 else 0))
        elif m in (0, 2):
            # next mode-3 start
            f = self.access_t % (154 * 456)
            if m == 2: nxt = 80 - x
            else:
                ny = y + 1
                nxt = 456 - x + 80 if ny < 144 else 10 ** 6
            self.pcslack[self.cpu_pc] = min(self.pcslack.get(self.cpu_pc, 10 ** 9), nxt)
            if nxt < self.slack:
                self.slack = nxt
                self.slack_at = (self.access_t, y, x, self.cpu_pc, self.l3[y] if y < 144 else 0)

    def advance(self, t1):
        """raise IF bits for events in (self.t, t1]"""
        t = self.t
        while True:
            f = t % (154 * 456)
            y, x = divmod(f, 456)
            # next candidate events on this line
            evs = []
            if y < 144:
                m0 = 80 + self.l3[y]
                if x < m0: evs.append(m0 - x)
            if y == 143:
                evs.append(456 - x)       # line 144 start = VBL
            evs.append(456 - x)
            d = min(evs)
            if t + d > t1: break
            t += d
            f = t % (154 * 456)
            y, x = divmod(f, 456)
            if y < 144 and x == 80 + self.l3[y] and (self.stat_en & 0x08): self.iflag |= 2; self.if1_t = t
            if y == 144 and x == 0: self.iflag |= 1
        self.t = t1

    def run(self, pc, limit=3_000_000):
        c = self.cpu
        c.pc = pc
        c.sp = 0xDFF0
        c.push(0xFFFF)
        start = self.t
        ei_pending = False
        while c.pc != 0xFFFF:
            if self.t - start > limit: raise RuntimeError('runaway')
            # interrupt dispatch
            if self.ime and (self.ie & self.iflag):
                pend = self.ie & self.iflag
                if pend & 1:
                    self.iflag &= ~1
                    self.advance(self.t + 20 + self.isr_t)
                    continue                 # reti: IME stays 1
                raise RuntimeError('dispatch of IF %02X at %04X' % (pend, c.pc))
            op = self.raw[c.pc]
            if op == 0x76:                   # halt
                c.pc += 1
                if not self.ime and (self.ie & self.iflag):
                    # halt bug: no wait, the next (1-byte) instruction runs twice
                    late = self.t - self.if1_t
                    self.late.append((late, c.pc - 1))
                    if self.raw[c.pc] not in (0x00, 0x12, 0x71, 0x70, 0x72, 0x73):
                        raise RuntimeError('halt bug before %02X at %04X' % (self.raw[c.pc], c.pc))
                    self.advance(self.t + 4)
                    save = c.pc
                    self.access_t = self.t + 4
                    self.advance(self.t + c.step())
                    c.pc = save
                    continue
                while not (self.ie & self.iflag):
                    self.advance(self.t + 4)
                self.advance(self.t + 4)
                continue
            if op == 0xF3: self.ime = 0; c.pc += 1; self.advance(self.t + 4); continue
            if op == 0xFB:
                c.pc += 1; self.advance(self.t + 4)
                # ei: enabled after the next instruction
                op2 = self.raw[c.pc]
                self.access_t = self.t + ACC_AT.get(op2, 4)
                cyc = c.step()
                self.advance(self.t + cyc)
                self.ime = 1
                continue
            if op == 0xD9:                   # reti
                self.ime = 1
            self.access_t = self.t + ACC_AT.get(op, 4)
            self.cpu_pc = c.pc
            self.pcs[c.pc] += 1
            at = c.pc
            cyc = c.step()
            self.pcc[at] += cyc
            self.advance(self.t + cyc)
        return self.t - start
