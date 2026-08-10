#!/usr/bin/env python3
"""Find the first instruction where our 68000 diverges from a reference trace.

  python3 tools/diff68k.py                    diff build/trace68k.txt vs the ref
  python3 tools/diff68k.py --extract          rebuild the reference extract
  python3 tools/diff68k.py --context 30       more lines of lead-up

Our trace comes from `build/mars --trace68k`; the reference comes from an
emulator whose tracer logs one line per instruction with the register state
*before* it executes.

The alignment, the classification of a difference and the reporting live in
`tools/tracediff.py`, which `tools/diffsh2.py` shares; what is here is the
68000: its line format, finding its reset in the logs, and its registers.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import tracediff                                       # noqa: E402
from m68k.decode import decode                         # noqa: E402

ROMS = "roms"
DEFAULT_ROM = "roms/Knuckles' Chaotix (JU) (32X) [!].32x"
REF_CACHE = "build/ref68k.txt"
OUR_TRACE = "build/trace68k.txt"
REF_LINES = 200000

FIELD = re.compile(r"^([da][0-7]|sp):([0-9a-f]{8})$")
OMITTED = re.compile(r"\[Omitted:\s*(\d+)\]")

NAMES = ["d%d" % i for i in range(8)] + ["a%d" % i for i in range(8)] + ["sp"]


def parse(path, want_text=False):
    """Read a trace into records. Non-instruction lines become markers.

    Field-split rather than matched with one big regular expression: the
    register block is 17 like-shaped fields, and a pattern general enough to
    skip a variable-width mnemonic in front of them backtracks catastrophically
    on the lines that are not instructions at all.
    """
    recs, markers, pending = [], {}, []
    gap = 0
    with open(path, "r", errors="replace") as f:
        for n, line in enumerate(f, 1):
            body = line.split("CPU Instruction: ", 1)[-1].strip()
            tok = body.split()
            if len(tok) < 20 or tok[0] != "CPU":
                m = OMITTED.search(body)
                if m:
                    gap += int(m.group(1))
                # An interrupt says a lot about where a run went, so keep it
                # attached to the next instruction.
                elif "Interrupt" in line:
                    pending.append(body)
                continue
            regs, sp, first = [], None, len(tok)
            for k, t in enumerate(tok[3:], 3):
                m = FIELD.match(t)
                if not m:
                    continue
                first = min(first, k)
                if m.group(1) == "sp":
                    sp = int(m.group(2), 16)
                else:
                    regs.append(int(m.group(2), 16))
            if len(regs) != 16 or sp is None:
                continue
            try:
                pc, op = int(tok[1], 16), int(tok[2], 16)
            except ValueError:
                continue
            text = " ".join(tok[3:first]) if want_text else ""
            recs.append(tracediff.Rec(pc, op, tuple(regs) + (sp,), tok[-1],
                                      text, n, gap))
            if pending:
                markers[n] = pending
                pending = []
            gap = 0
    return recs, markers


# --------------------------------------------------------------- extraction --
def find_reset_pc(rom_path):
    with open(rom_path, "rb") as f:
        head = f.read(8)
    return int.from_bytes(head[4:8], "big") & 0xFFFFFF


def extract(rom_path, out_path, limit):
    """Locate the 68000 reset in the reference logs and copy the run forward.

    Two things make this more than a grep. The logs hold every CPU interleaved,
    so the 68000 is a couple of percent of the lines. And they are 1 GB segments
    of one long run, so the reset is wherever it happens to fall — here a way
    into the fifth segment, because tracing was started with the game already
    running and the console reset afterwards. The segments do run on from each
    other, though: one ends mid-instruction-stream and the next picks it up at
    the branch target, so the trace is followed across them until `limit` lines.
    """
    pc = find_reset_pc(rom_path)
    first, at, logs = find_reset(pc)

    n = 0
    with open(out_path, "w") as out:
        for k in range(first, len(logs)):
            with open(logs[k], "rb") as f:
                if k == first:
                    f.seek(at)
                for raw in f:
                    line = raw.decode("latin1")
                    if line.startswith("CPU "):
                        out.write(line)
                        n += 1
                        if n >= limit:
                            break
            print("  through %s: %d lines" % (os.path.basename(logs[k]), n))
            if n >= limit:
                break
    print("  wrote %s (%d lines)" % (out_path, n))


def find_reset(pc):
    """Which log segment holds the 68000's reset, and where in it.

    Shared with `tools/diffsh2.py`: the SH-2 streams are located the same way,
    because the reset that matters is the console's and the 68000's reset line
    is the one landmark with a known address.
    """
    needle = ("CPU  %06x" % pc).encode()
    logs = sorted(os.path.join(ROMS, f) for f in os.listdir(ROMS)
                  if f.endswith(".log"))
    if not logs:
        sys.exit("no reference logs in %s/" % ROMS)
    for k, path in enumerate(logs):
        at = tracediff.find_in(path, needle)
        if at >= 0:
            print("  %s: reset PC 0x%06X at byte %d"
                  % (os.path.basename(path), pc, at))
            return k, at, logs
        print("  %-52s no reset (%d MB)"
              % (os.path.basename(path), os.path.getsize(path) >> 20))
    sys.exit("reset PC 0x%06X not found in any log" % pc)


# ------------------------------------------------------------- annotation ---
class Disasm:
    """Annotate an address using the project's own validated decoder."""

    def __init__(self, rom_path):
        with open(rom_path, "rb") as f:
            self.rom = f.read()

    def offset(self, a):
        if 0x880000 <= a < 0x900000:
            off = a - 0x880000
        elif 0x900000 <= a < 0xA00000:
            off = a - 0x900000            # bank 0
        elif a < 0x400000:
            off = a
        else:
            return None
        return off if off + 1 < len(self.rom) else None

    def at(self, a):
        off = self.offset(a)
        if off is None:
            return "(not in ROM)"
        def fetch(x):
            o = self.offset(x)
            return 0 if o is None else int.from_bytes(self.rom[o:o + 2], "big")
        try:
            return decode(fetch, a).text()
        except Exception:
            return "(undecodable)"


def show_regs(rec):
    r = rec.regs
    return ["       d " + " ".join("%08x" % v for v in r[:8]),
            "       a " + " ".join("%08x" % v for v in r[8:16]),
            "       sp:%08x  %s" % (r[16], rec.flags)]


# ------------------------------------------------------------------ report --
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=DEFAULT_ROM)
    ap.add_argument("--ref", default=REF_CACHE)
    ap.add_argument("--ours", default=OUR_TRACE)
    ap.add_argument("--extract", action="store_true",
                    help="rebuild the reference extract from roms/*.log")
    ap.add_argument("--limit", type=int, default=REF_LINES,
                    help="reference lines to extract")
    ap.add_argument("--ref-lines", type=int, default=0,
                    help="compare only the first N reference lines; the boot "
                         "ends at 20213, before the first vblank interrupt")
    ap.add_argument("--context", type=int, default=12,
                    help="reference instructions of lead-up to print")
    ap.add_argument("--window", type=int, default=40000,
                    help="how far forward in our stream to look for a match")
    ap.add_argument("--hold", type=int, default=4,
                    help="instructions that must keep matching to accept a spot")
    ap.add_argument("--skip", type=int, default=400,
                    help="reference instructions we may never execute and still"
                         " call it a rejoin")
    ap.add_argument("--rows", type=int, default=40,
                    help="summary rows to print before truncating")
    ap.add_argument("--detail", type=int, default=2,
                    help="non-fatal divergences to print in full")
    args = ap.parse_args()

    if args.extract or not os.path.exists(args.ref):
        print("extracting reference trace:")
        extract(args.rom, args.ref, args.limit)

    dis = Disasm(args.rom)
    cpu = tracediff.Cpu(NAMES, 6, dis.at, show_regs)
    ref, marks = parse(args.ref, want_text=True)
    ours, _ = parse(args.ours)
    if args.ref_lines:
        ref = [r for r in ref if r.lineno <= args.ref_lines]
    print("reference %d instructions, ours %d" % (len(ref), len(ours)))
    if not ref or not ours:
        sys.exit("nothing to compare")

    divs, agreed = tracediff.diff(ref, ours, args.window, args.hold, args.skip)
    return tracediff.report(cpu, ref, ours, marks, divs, agreed, args)


if __name__ == "__main__":
    sys.exit(main())
