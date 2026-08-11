#!/usr/bin/env python3
"""How much each CPU executes in one frame of the reference run.

  python3 tools/refrate.py            eight frames, from the console reset
  python3 tools/refrate.py 40         forty of them

Every rate in the scheduler is a division: a frame is a fixed number of cycles,
and turning that into a slice of *instructions* needs to know what an
instruction costs. Guessing that number has been wrong twice — `recomp_cpi` was
8 against a true 11.4, and the SH-2s ran at a flat 2 cycles an instruction
against a true 1.6 and 1.0 — and both times the trace found it rather than the
manual, because what governs is this game's actual mix and not the cheapest
encoding in the book.

So ask the logs. They interleave every CPU and mark the 68000's vertical
interrupts, which is a frame boundary all three share; counting the lines
between two of those marks — the collapsed runs included, or an idle loop
vanishes — is the measurement directly. Divide the frame's cycles by it.

The first interval is the tail of whatever the run was doing when tracing
started and the second is still the boot; from the third on they are steady.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import diff68k                                          # noqa: E402

DEFAULT_ROM = "roms/Knuckles' Chaotix (JU) (32X) [!].32x"
OMITTED = re.compile(rb"\[Omitted:\s*(\d+)\]")
TAGS = (b"CPU", b"SHM", b"SHS", b"Z80")

# The Genesis master clock over seven, across a frame of 262 lines of 3,420
# master clocks — 59.9227 Hz, not 60 — and the SH-2s clocked at exactly three
# times it. Mirrors CYCLES_PER_FRAME in src/mars_main.c, which carries the
# derivation and what it was measured against.
CYCLES_PER_FRAME = 128006
SH2_MULTIPLIER = 3


def frames(rom_path, want):
    """Instructions per CPU between consecutive vertical interrupts."""
    first, at, logs = diff68k.find_reset(diff68k.find_reset_pc(rom_path))
    n = {t: 0 for t in TAGS}
    last, rows = None, []
    for k in range(first, len(logs)):
        with open(logs[k], "rb") as f:
            if k == first:
                f.seek(at)
            for raw in f:
                tag = raw[:3]
                if tag not in n:
                    continue
                body = raw.split(b" Instruction: ", 1)[-1]
                m = OMITTED.search(body)
                if m:
                    # A collapsed run is instructions the CPU really ran, and
                    # the idle loops this is measuring are almost all of it.
                    n[tag] += int(m.group(1))
                elif b"Interrupt: Vblank" in body:
                    if last is not None:
                        rows.append({t: n[t] - last[t] for t in n})
                        if len(rows) >= want:
                            return rows
                    last = dict(n)
                else:
                    n[tag] += 1
    return rows


def main():
    want = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    rom = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_ROM
    rows = frames(rom, want)
    if not rows:
        sys.exit("no vertical interrupts found after the reset")

    print("\ninstructions between two of the 68000's vertical interrupts:")
    print("  %-8s %s" % ("frame", "  ".join("%8s" % t.decode() for t in TAGS)))
    for i, r in enumerate(rows, 1):
        print("  %-8d %s" % (i, "  ".join("%8d" % r[t] for t in TAGS)))

    # The first two intervals are the tail of the previous run and the boot.
    steady = rows[2:] or rows
    print("\nmean over %d steady frame(s), and the cycles an instruction cost:"
          % len(steady))
    for t in TAGS:
        mean = sum(r[t] for r in steady) / len(steady)
        cyc = CYCLES_PER_FRAME * (SH2_MULTIPLIER if t in (b"SHM", b"SHS") else 1)
        print("  %s %10.1f   %s" % (t.decode(), mean,
                                    "%.3f cycles/instruction" % (cyc / mean)
                                    if mean else "(never runs)"))


if __name__ == "__main__":
    sys.exit(main())
