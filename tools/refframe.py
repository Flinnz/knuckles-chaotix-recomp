#!/usr/bin/env python3
"""Rebuild what the real machine had in its 32X frame buffer, from the trace.

  python3 tools/refframe.py                 replay the master, write build/ref32x.bin
  python3 tools/refframe.py --ppm F.ppm     and render it
  python3 tools/refframe.py --compare F     compare against a --dump-32x of ours

The rendering path is the one part of this project with no oracle. The decoder
is checked against objdump, the classification by round-trip, the CPUs against
an instruction trace — but nothing has ever said the pixels are right. Both
pictures are *plausible*, which is not the same as verified, and the packed
pixel decode, the palette conversion and the frame-select polarity all rest on
it.

An instruction trace looks like the wrong tool for that, being a log of CPU
state rather than of memory. It is not. Every store the SH-2 makes is in there
with the register state that produced it, so the writes can be replayed: the
address and the datum are both functions of registers the log prints. Replaying
the master's 5.6 million instructions reconstructs the frame buffer the real
machine had, byte for byte, with no emulator involved.

Two things it cannot do, and the first was not obvious. **The reference tracer
collapses tight loops**, printing the first iteration and eliding the rest, so
the stores inside them were never logged and cannot be replayed. The line-table
writer at 0x060031B8 is the clearest case: 256 iterations per invocation, six
lines in the whole extract, all of them the first iteration with r8 still
0x24000200. A reconstruction is therefore a *lower bound* on what was written,
and where a loop is elided the arithmetic has to be read out of the code
instead — which is what settled the line table here. And it cannot say which
buffer was on screen, that being a hardware fact no CPU trace records.

What it does establish is the contents: every byte the log does account for,
at the address the registers put it, with no emulator in the loop.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

REF = "build/ref-long-shm.txt"
OUT = "build/ref32x.bin"

FB = 0x20000            # one frame buffer, 128 KB
SDRAM = 0x40000
CACHE = 0x1000

# The reference prints one instruction per line with the register state before
# it runs. Only the stores matter here, and its disassembly names them in five
# shapes — counted over the whole extract, there are no others.
STORE = re.compile(
    r"^(mov|sts|stc)\.([bwl])\s+"
    r"(r\d+|pr|sr|gbr|vbr|mach|macl)\s*,\s*@"
    r"(?:(-?)(r\d+)|\((?:(0x[0-9a-f]+|\d+)|(r\d+))\s*,\s*(r\d+|gbr)\))$")

FIELD = re.compile(r"^(r\d+|sr|gbr|vbr|mach|macl|pr):([0-9a-f]{8})$")
SIZE = {"b": 1, "w": 2, "l": 4}


def canon(a):
    """The runtime's own folding, and the hardware's: below 0x40000000 the
    cached and cache-through views are mirrors of each other."""
    return a & 0x1FFFFFFF if a < 0x40000000 else a


class Machine:
    """Just enough address space to hold the picture.

    Two frame buffers, because FBCR bit 0 selects between them and the whole
    point is to be able to tell them apart. SDRAM and the cache array are kept
    because the frame buffer's contents are computed out of them and a store
    that lands there is not a store that was lost.
    """

    def __init__(self):
        self.fb = [bytearray(FB), bytearray(FB)]
        self.sdram = bytearray(SDRAM)
        self.cache = bytearray(CACHE)
        self.cram = bytearray(0x200)
        self.fbctl = 0
        self.bitmap_mode = 0
        self.fill_len = self.fill_start = self.fill_data = 0
        self.counts = {"fb": 0, "fb_overwrite": 0, "sdram": 0, "cache": 0,
                       "cram": 0, "reg": 0, "onchip": 0, "purge": 0,
                       "fill": 0, "elsewhere": 0}

    def draw(self):
        """Which buffer the CPUs reach — the one not being displayed."""
        return self.fb[(self.fbctl & 1) ^ 1]

    def store(self, addr, size, val):
        a = canon(addr)
        for k in range(size):
            b = (val >> (8 * (size - 1 - k))) & 0xFF
            self._byte(a + k, b, a)

    def _byte(self, a, b, base):
        if 0x06000000 <= a < 0x06000000 + SDRAM:
            self.sdram[a - 0x06000000] = b
            if a == base: self.counts["sdram"] += 1
        elif 0x04000000 <= a < 0x04020000:
            self.draw()[a - 0x04000000] = b
            if a == base: self.counts["fb"] += 1
        elif 0x04020000 <= a < 0x04040000:
            # The overwrite image drops zero bytes instead of storing them,
            # which is how a sprite is drawn without a mask.
            if b:
                self.draw()[a - 0x04020000] = b
            if a == base: self.counts["fb_overwrite"] += 1
        elif 0xC0000000 <= a < 0xC0000000 + CACHE:
            self.cache[a - 0xC0000000] = b
            if a == base: self.counts["cache"] += 1
        elif 0x4200 <= a < 0x4400:
            self.cram[a - 0x4200] = b
            if a == base: self.counts["cram"] += 1
        elif a < 0x10000:
            if a == base: self.counts["reg"] += 1
            self._reg(a, b)
        elif a >= 0xFFFFFE00:
            # The SH-2's own peripheral block: FRT, WDT, DMAC, serial.
            if a == base: self.counts["onchip"] += 1
        elif 0x40000000 <= a < 0x60000000:
            # The SH7604's associative purge area — cache management, and a
            # no-op for a model with no cache.
            if a == base: self.counts["purge"] += 1
        else:
            if a == base: self.counts["elsewhere"] += 1

    def _reg(self, a, b):
        """The registers that decide how the frame buffer is read, and the
        autofill, which writes to it without the CPU storing anything."""
        if a == 0x410A: self.fbctl = (self.fbctl & 0xFF) | (b << 8)
        elif a == 0x410B: self.fbctl = (self.fbctl & 0xFF00) | b
        elif a == 0x4100: self.bitmap_mode = (self.bitmap_mode & 0xFF) | (b << 8)
        elif a == 0x4101: self.bitmap_mode = (self.bitmap_mode & 0xFF00) | b
        elif a == 0x4104: self.fill_len = (self.fill_len & 0xFF) | (b << 8)
        elif a == 0x4105: self.fill_len = (self.fill_len & 0xFF00) | b
        elif a == 0x4106: self.fill_start = (self.fill_start & 0xFF) | (b << 8)
        elif a == 0x4107: self.fill_start = (self.fill_start & 0xFF00) | b
        elif a == 0x4108: self.fill_data = (self.fill_data & 0xFF) | (b << 8)
        elif a == 0x4109:
            # Writing the low half of the data register is what starts it.
            self.fill_data = (self.fill_data & 0xFF00) | b
            self.autofill()

    def autofill(self):
        """A word address that increments in its low 8 bits only, so the fill
        wraps inside a 256-word block. Same as src/mem32x.c."""
        addr = self.fill_start
        fb = self.draw()
        for _ in range(self.fill_len + 1):
            o = (addr * 2) & 0x1FFFE
            fb[o] = (self.fill_data >> 8) & 0xFF
            fb[o + 1] = self.fill_data & 0xFF
            addr = (addr & 0xFF00) | ((addr + 1) & 0xFF)
        self.counts["fill"] += 1


def replay(path, tag, limit=0):
    """Stream the log, applying every store. The file is gigabytes, so nothing
    is kept but the machine itself."""
    m = Machine()
    n = stores = 0
    names = (["r%d" % i for i in range(16)]
             + ["sr", "gbr", "vbr", "mach", "macl", "pr"])
    with open(path, "r", errors="replace") as f:
        for line in f:
            if not line.startswith(tag):
                continue
            body = line.split(" Instruction: ", 1)[-1]
            tok = body.split()
            if len(tok) < 3 or tok[0] != tag:
                continue
            reg, first = {}, len(tok)
            for k, t in enumerate(tok[2:], 2):
                mm = FIELD.match(t)
                if mm:
                    first = min(first, k)
                    reg[mm.group(1)] = int(mm.group(2), 16)
            if len(reg) != len(names):
                continue
            n += 1
            if limit and n > limit:
                break
            text = " ".join(tok[2:first])
            mo = STORE.match(text)
            if not mo:
                continue
            _, sz, src, pre, rn, disp, idx, base = mo.groups()
            size = SIZE[sz]
            val = reg[src]
            if rn is not None:                       # @rN or @-rN
                addr = reg[rn] - (size if pre else 0)
            elif idx is not None:                    # @(r0,rN)
                addr = reg[idx] + reg[base]
            else:                                    # @(disp,rN) or @(disp,gbr)
                addr = reg[base] + int(disp, 0)
            m.store(addr & 0xFFFFFFFF, size, val)    # the SH-2's adder is 32-bit
            stores += 1
    return m, n, stores


def write_dump(m, path):
    with open(path, "wb") as f:
        f.write(m.fb[0]); f.write(m.fb[1]); f.write(m.cram)
        f.write(bytes([m.fbctl >> 8, m.fbctl & 0xFF,
                       m.bitmap_mode >> 8, m.bitmap_mode & 0xFF]))


def read_dump(path):
    m = Machine()
    with open(path, "rb") as f:
        d = f.read()
    if len(d) < 2 * FB + 0x200 + 4:
        sys.exit("%s is %d bytes, expected %d" % (path, len(d), 2 * FB + 0x204))
    m.fb[0] = bytearray(d[:FB])
    m.fb[1] = bytearray(d[FB:2 * FB])
    m.cram = bytearray(d[2 * FB:2 * FB + 0x200])
    m.fbctl = (d[2 * FB + 0x200] << 8) | d[2 * FB + 0x201]
    m.bitmap_mode = (d[2 * FB + 0x202] << 8) | d[2 * FB + 0x203]
    return m


def describe(fb):
    """What is in a buffer, in the terms the runtime's own report uses."""
    nz = sum(1 for b in fb if b)
    table = [(fb[i * 2] << 8) | fb[i * 2 + 1] for i in range(224)]
    set_ = sum(1 for t in table if t)
    return nz, set_, len(set(t for t in table if t))


def render(m, which, path):
    """Packed pixel: a per-line table of 16-bit word offsets, then one byte a
    pixel indexing the 256-entry palette. Written the same way the runtime's
    own renderer reads it, so a disagreement is in the data, not the reading."""
    fb = m.fb[which]
    W, H = 320, 224
    px = bytearray(W * H * 3)
    for y in range(H):
        off = ((fb[y * 2] << 8) | fb[y * 2 + 1]) * 2
        for x in range(W):
            o = off + x
            idx = fb[o] if o < FB else 0
            c = (m.cram[idx * 2] << 8) | m.cram[idx * 2 + 1]
            r = (c & 0x1F) << 3
            g = ((c >> 5) & 0x1F) << 3
            b = ((c >> 10) & 0x1F) << 3
            p = (y * W + x) * 3
            px[p] = r; px[p + 1] = g; px[p + 2] = b
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (W, H))
        f.write(px)


def compare(a, b):
    """Ours against the reference's, both buffers each way round — which is
    what tells the frame-select polarity apart from the contents."""
    print("\n%-22s %-28s %s" % ("", "reference", "ours"))
    for i in (0, 1):
        an, at, ad = describe(a.fb[i])
        bn, bt, bd = describe(b.fb[i])
        print("  fb[%d] non-zero bytes  %-28d %d" % (i, an, bn))
        print("        line table       %-28s %s"
              % ("%d/224 lines, %d distinct" % (at, ad),
                 "%d/224 lines, %d distinct" % (bt, bd)))
    print("  cram non-zero bytes    %-28d %d"
          % (sum(1 for x in a.cram if x), sum(1 for x in b.cram if x)))
    print("  fbctl                  %-28s %s"
          % ("0x%04X" % a.fbctl, "0x%04X" % b.fbctl))
    print("  bitmap mode            %-28s %s"
          % ("0x%04X" % a.bitmap_mode, "0x%04X" % b.bitmap_mode))

    print("\n  identical bytes, each buffer against each:")
    for i in (0, 1):
        for j in (0, 1):
            same = sum(1 for x, y in zip(a.fb[i], b.fb[j]) if x == y)
            print("    ref fb[%d] vs ours fb[%d]  %7d / %d  (%.1f%%)"
                  % (i, j, same, FB, 100.0 * same / FB))
    same = sum(1 for x, y in zip(a.cram, b.cram) if x == y)
    print("    cram                   %7d / %d  (%.1f%%)"
          % (same, len(a.cram), 100.0 * same / len(a.cram)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default=REF)
    ap.add_argument("--cpu", default="master", choices=("master", "slave"))
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after N reference instructions")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--ppm", default=None)
    ap.add_argument("--compare", default=None,
                    help="a file written by `build/mars --dump-32x`")
    args = ap.parse_args()

    tag = "SHM" if args.cpu == "master" else "SHS"
    if not os.path.exists(args.ref):
        sys.exit("%s not found — build it with\n"
                 "  python3 -c \"import sys; sys.path.insert(0,'tools'); "
                 "import diffsh2; diffsh2.extract(ROM, 8000000, "
                 "diffsh2.cache_paths('build/ref-long-'))\"" % args.ref)

    print("replaying %s ..." % args.ref)
    m, n, stores = replay(args.ref, tag, args.limit)
    print("  %d instruction(s), %d store(s)" % (n, stores))
    for k in ("fb", "fb_overwrite", "sdram", "cache", "cram", "reg",
              "elsewhere"):
        if m.counts[k]:
            print("    %-13s %d" % (k, m.counts[k]))
    for i in (0, 1):
        nz, t, d = describe(m.fb[i])
        print("  fb[%d]: %d non-zero byte(s), line table %d/224 set, "
              "%d distinct" % (i, nz, t, d))
    print("  cram: %d/512 non-zero, fbctl 0x%04X, bitmap mode 0x%04X"
          % (sum(1 for x in m.cram if x), m.fbctl, m.bitmap_mode))

    write_dump(m, args.out)
    print("  wrote %s" % args.out)
    if args.ppm:
        render(m, (m.fbctl & 1), args.ppm)
        print("  wrote %s (fb[%d], the displayed one)" % (args.ppm, m.fbctl & 1))
    if args.compare:
        compare(m, read_dump(args.compare))
    return 0


if __name__ == "__main__":
    sys.exit(main())
