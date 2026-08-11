#!/usr/bin/env python3
"""Find the first instruction where our Z80 diverges from the reference trace.

  python3 tools/diffz80.py                    diff build/tracez80.txt vs the ref
  python3 tools/diffz80.py --extract          rebuild the reference extract

Our trace comes from `build/mars --trace-z80`; the reference is the same
emulator log the 68000 and both SH-2s are held to, filtered to its APU lines.
The alignment and the reporting are `tools/tracediff.py`, shared with the other
three; what is here is the Z80 — its line format, its registers, and a
disassembler for the annotations, since the Z80's code is not in the cartridge
in any form a static tool could have found. It is written into RAM by the 68000
at run time, so the only place to read it from is our own Z80 RAM after a run,
which `--ram` names.

The extract starts at the 68000's reset, the same landmark every other diff
here uses, and the Z80 is already running when it happens: the first lines are
the stub the cartridge leaves in place — `di / im 1 / jp 0x005B` — and the real
driver appears later, after the 68000 has uploaded it and released reset.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import tracediff                                       # noqa: E402
import diff68k                                         # noqa: E402

DEFAULT_ROM = diff68k.DEFAULT_ROM
REF_CACHE = "build/refz80.txt"
OUR_TRACE = "build/tracez80.txt"
DEFAULT_RAM = "build/z80ram.bin"
REF_LINES = 200000

FIELD = re.compile(r"^(AF|BC|DE|HL|IX|IY|SP):([0-9a-f]{4})$")
IFF = re.compile(r"^IFF:(\d)(\d)$")
IM = re.compile(r"^IM:(\d)$")
OMITTED = re.compile(r"\[Omitted:\s*(\d+)\]")

NAMES = ["af", "bc", "de", "hl", "ix", "iy", "sp", "iff", "im"]


def parse(path, want_text=False):
    """Read one trace into records.

    Ours and the reference's differ only in the column between the address and
    the registers — theirs a mnemonic, ours the opcode byte — so both are read
    by scanning for the `NAME:value` fields rather than by counting tokens.
    """
    recs, markers, pending = [], {}, []
    gap = 0
    with open(path, "r", errors="replace") as f:
        for n, line in enumerate(f, 1):
            body = line.split("APU Instruction: ", 1)[-1].strip()
            tok = body.split()
            if not tok or tok[0] != "APU":
                m = OMITTED.search(body)
                if m:
                    gap += int(m.group(1))
                elif "Interrupt" in line:
                    pending.append(body)
                continue
            vals, first = {}, len(tok)
            for k, t in enumerate(tok[2:], 2):
                m = FIELD.match(t)
                if m:
                    first = min(first, k)
                    vals[m.group(1).lower()] = int(m.group(2), 16)
                    continue
                m = IFF.match(t)
                if m:
                    first = min(first, k)
                    vals["iff"] = int(m.group(1)) * 2 + int(m.group(2))
                    continue
                m = IM.match(t)
                if m:
                    first = min(first, k)
                    vals["im"] = int(m.group(1))
            if len(vals) != len(NAMES):
                continue
            try:
                pc = int(tok[1], 16)
            except ValueError:
                continue
            text = " ".join(tok[2:first]) if want_text else ""
            recs.append(tracediff.Rec(pc, None, tuple(vals[k] for k in NAMES),
                                      "", text, n, gap))
            if pending:
                markers[n] = pending
                pending = []
            gap = 0
    return recs, markers


def extract(rom_path, out_path, limit):
    """Copy the reference's APU stream forward from the console reset."""
    pc = diff68k.find_reset_pc(rom_path)
    first, at, logs = diff68k.find_reset(pc)
    n = 0
    with open(out_path, "w") as out:
        for k in range(first, len(logs)):
            with open(logs[k], "rb") as f:
                if k == first:
                    f.seek(at)
                for raw in f:
                    line = raw.decode("latin1")
                    if line.startswith("APU"):
                        out.write(line)
                        n += 1
                        if n >= limit:
                            break
            print("  through %s: %d lines" % (os.path.basename(logs[k]), n))
            if n >= limit:
                break
    print("  wrote %s (%d lines)" % (out_path, n))


# ------------------------------------------------------------- annotation ---
# Enough of a disassembler to label a divergence. Not a decoder to be validated
# against anything: the trace comparison is the check, and what this is for is
# saying which instruction the two runs disagreed at.
R8 = ["b", "c", "d", "e", "h", "l", "(hl)", "a"]
RP = ["bc", "de", "hl", "sp"]
RP2 = ["bc", "de", "hl", "af"]
CC = ["nz", "z", "nc", "c", "po", "pe", "p", "m"]
ALU = ["add a,", "adc a,", "sub ", "sbc a,", "and ", "xor ", "or ", "cp "]
ROT = ["rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl"]
ACC = ["rlca", "rrca", "rla", "rra", "daa", "cpl", "scf", "ccf"]


class Disasm:
    """Annotate an address out of the Z80 RAM a run left behind."""

    def __init__(self, path):
        self.ram = b"\0" * 0x2000
        if path and os.path.exists(path):
            with open(path, "rb") as f:
                self.ram = f.read()

    def b(self, a):
        return self.ram[a & 0x1FFF] if a < 0x4000 else 0

    def w(self, a):
        return self.b(a) | (self.b(a + 1) << 8)

    def at(self, a):
        if a >= 0x4000:
            return "(outside RAM)"
        op = self.b(a)
        if op == 0xCB:
            o = self.b(a + 1)
            x, y, z = o >> 6, (o >> 3) & 7, o & 7
            return ["%s %s" % (ROT[y], R8[z]), "bit %d,%s" % (y, R8[z]),
                    "res %d,%s" % (y, R8[z]), "set %d,%s" % (y, R8[z])][x]
        if op in (0xDD, 0xFD):
            return "%s %s" % ("ix" if op == 0xDD else "iy", self.at(a + 1))
        if op == 0xED:
            return "ed %02x" % self.b(a + 1)
        x, y, z = op >> 6, (op >> 3) & 7, op & 7
        p, q = y >> 1, y & 1
        if x == 1:
            return "halt" if op == 0x76 else "ld %s,%s" % (R8[y], R8[z])
        if x == 2:
            return ALU[y] + R8[z]
        if x == 0:
            if z == 0:
                return ["nop", "ex af,af'", "djnz $%04x" % ((a + 2 + ((self.b(a + 1) ^ 0x80) - 0x80)) & 0xFFFF),
                        "jr $%04x" % ((a + 2 + ((self.b(a + 1) ^ 0x80) - 0x80)) & 0xFFFF)][y] \
                    if y < 4 else "jr %s,$%04x" % (CC[y - 4],
                                                   (a + 2 + ((self.b(a + 1) ^ 0x80) - 0x80)) & 0xFFFF)
            if z == 1:
                return "add hl,%s" % RP[p] if q else "ld %s,$%04x" % (RP[p], self.w(a + 1))
            if z == 2:
                return ["ld (bc),a", "ld a,(bc)", "ld (de),a", "ld a,(de)",
                        "ld ($%04x),hl" % self.w(a + 1), "ld hl,($%04x)" % self.w(a + 1),
                        "ld ($%04x),a" % self.w(a + 1), "ld a,($%04x)" % self.w(a + 1)][y]
            if z == 3:
                return "%s %s" % ("dec" if q else "inc", RP[p])
            if z == 4:
                return "inc %s" % R8[y]
            if z == 5:
                return "dec %s" % R8[y]
            if z == 6:
                return "ld %s,$%02x" % (R8[y], self.b(a + 1))
            return ACC[y]
        if z == 0:
            return "ret %s" % CC[y]
        if z == 1:
            return "pop %s" % RP2[p] if not q else ["ret", "exx", "jp (hl)", "ld sp,hl"][p]
        if z == 2:
            return "jp %s,$%04x" % (CC[y], self.w(a + 1))
        if z == 3:
            return ["jp $%04x" % self.w(a + 1), "cb", "out ($%02x),a" % self.b(a + 1),
                    "in a,($%02x)" % self.b(a + 1), "ex (sp),hl", "ex de,hl",
                    "di", "ei"][y]
        if z == 4:
            return "call %s,$%04x" % (CC[y], self.w(a + 1))
        if z == 5:
            return "push %s" % RP2[p] if not q else \
                ["call $%04x" % self.w(a + 1), "dd", "ed", "fd"][p]
        if z == 6:
            return ALU[y] + "$%02x" % self.b(a + 1)
        return "rst $%02x" % (y * 8)


def show_regs(rec):
    af, bc, de, hl, ix, iy, sp, iff, im = rec.regs
    f = rec.regs[0] & 0xFF
    bits = "".join(c if f & (1 << (7 - i)) else c.lower()
                   for i, c in enumerate("SZYHXVNC"))
    return ["       af:%04x bc:%04x de:%04x hl:%04x" % (af, bc, de, hl),
            "       ix:%04x iy:%04x sp:%04x  iff:%d%d im:%d  %s"
            % (ix, iy, sp, iff >> 1, iff & 1, im, bits)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=DEFAULT_ROM)
    ap.add_argument("--ref", default=REF_CACHE)
    ap.add_argument("--ours", default=OUR_TRACE)
    ap.add_argument("--ram", default=DEFAULT_RAM,
                    help="the Z80 RAM a run left behind, for the annotations")
    ap.add_argument("--extract", action="store_true")
    ap.add_argument("--limit", type=int, default=REF_LINES)
    ap.add_argument("--ref-lines", type=int, default=0)
    ap.add_argument("--context", type=int, default=12)
    ap.add_argument("--window", type=int, default=200000)
    ap.add_argument("--hold", type=int, default=4)
    ap.add_argument("--skip", type=int, default=400)
    ap.add_argument("--rows", type=int, default=40)
    ap.add_argument("--detail", type=int, default=2)
    ap.add_argument("--detail-at", default="", metavar="PC[,PC...]")
    args = ap.parse_args()
    args.detail_at = {int(a, 16) for a in args.detail_at.split(",") if a}

    if args.extract or not os.path.exists(args.ref):
        print("extracting reference trace:")
        extract(args.rom, args.ref, args.limit)

    dis = Disasm(args.ram)
    cpu = tracediff.Cpu(NAMES, 4, dis.at, show_regs)
    ref, marks = parse(args.ref, want_text=True)
    ours, _ = parse(args.ours)
    # Our tracer collapses a run of one instruction in one state the way the
    # reference collapses its own; fold each gap back onto the record before it
    # or a collapsed spin reads as having run once.
    for k in range(1, len(ours)):
        if ours[k].gap:
            ours[k - 1].ran += ours[k].gap
            ours[k].gap = 0
    if args.ref_lines:
        ref = [r for r in ref if r.lineno <= args.ref_lines]
    print("reference %d instructions, ours %d" % (len(ref), len(ours)))
    if not ref or not ours:
        sys.exit("nothing to compare")

    divs, agreed = tracediff.diff(ref, ours, args.window, args.hold, args.skip)
    return tracediff.report(cpu, ref, ours, marks, divs, agreed, args)


if __name__ == "__main__":
    sys.exit(main())
