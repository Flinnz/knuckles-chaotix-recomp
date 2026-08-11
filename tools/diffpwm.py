#!/usr/bin/env python3
"""Compare the samples our 32X plays against the reference's, one for one.

  python3 tools/diffpwm.py                    the default extract
  python3 tools/diffpwm.py --ref build/ref-long-shs.txt      a longer one

Our side comes from `build/mars --trace-pwm FILE`, which logs every word pushed
into a PWM sample FIFO with the CPU that pushed it. The reference side is
*derived* rather than looked up: any instruction in the slave's log that stores
a word to 0x20004034, 0x20004036 or 0x20004038 is a sample, and the register
state the log prints before it executes says both where it is going and what
value it is. Nothing here knows the sound driver's addresses, so this keeps
working if the game ever plays a sample from somewhere else.

The two streams are compared per FIFO, in order, which is the whole of the
audio: a PWM sample is a pulse width and the unit takes them out one a cycle, so
if the sequences agree the sound does.

Only the slave's writes are compared. The 68000 clears the unit in its own 32X
init and pushes one zero through the mono port at 0x88072C, which lands in both
FIFOs — and which no slave log can contain, because the reference's logs are per
CPU. Those are reported separately rather than aligned away.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import diffsh2                                           # noqa: E402

OUR_TRACE = "build/tracepwm.txt"
DEFAULT_REF = diffsh2.REF_CACHE["slave"]

# The three sample ports, in the SH-2's own view of the register block.
PORTS = {0x20004034: "L", 0x20004036: "R", 0x20004038: "M"}

# `mov.w rS,@rD`, `mov.w rS,@(0xNN,rD)` and the GBR form, which is every way an
# SH-2 can put a word somewhere that is not the stack.
#
# The displacement the log prints is already in bytes, not the raw 4-bit field:
# the driver's own `mov.l r0,@(0x28,r14)` could not be a field at all, since
# those only reach 15. So nothing here scales it.
ST_REG = re.compile(r"^mov\.([bwl])\s+(r\d+),@(r\d+)$")
ST_DISP = re.compile(r"^mov\.([bwl])\s+(r\d+),@\(0x([0-9a-f]+),(r\d+|gbr)\)$")

OUR_LINE = re.compile(r"^([LR]) ([0-9A-Fa-f]{4}) (.)$")


def reg(regs, name):
    return regs[diffsh2.NAMES.index(name)]


def ref_stream(path):
    """Every sample the reference's slave pushed, in order, as (port, value).

    Also counts stores to a port that are not word-wide, which the unit does not
    have and which would mean this reading of the log is wrong somewhere.
    """
    recs, _ = diffsh2.parse(path, "SHS", want_text=True)
    out, odd = [], 0
    for r in recs:
        text = " ".join(r.text.split())
        m = ST_REG.match(text)
        if m:
            size, src, dst = m.group(1), m.group(2), m.group(3)
            addr = reg(r.regs, dst)
        else:
            m = ST_DISP.match(text)
            if not m:
                continue
            size, src, disp, dst = m.group(1), m.group(2), int(m.group(3), 16), \
                m.group(4)
            addr = reg(r.regs, dst) + disp
        if addr not in PORTS:
            continue
        if size != "w":
            odd += 1
            continue
        out.append((PORTS[addr], reg(r.regs, src) & 0xFFFF))
    return out, odd, len(recs)


def our_stream(path):
    """Ours, split by the CPU that wrote it."""
    by_cpu = {}
    with open(path) as f:
        for line in f:
            m = OUR_LINE.match(line.strip())
            if m:
                by_cpu.setdefault(m.group(3), []).append(
                    (m.group(1), int(m.group(2), 16)))
    return by_cpu


def describe(seq):
    """What a sample stream is, in one line. A stream that never changes value
    is silence whatever value it holds, which is what the reference's is."""
    if not seq:
        return "empty"
    lo, hi = min(seq), max(seq)
    changes = sum(1 for a, b in zip(seq, seq[1:]) if a != b)
    return "%d samples, %04X-%04X, changing %d time(s)%s" % (
        len(seq), lo, hi, changes, "" if changes > 2 else " — silence")


def split(seq, port):
    return [v for p, v in seq if p == port]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ours", default=OUR_TRACE)
    ap.add_argument("--ref", default=DEFAULT_REF)
    ap.add_argument("--rows", type=int, default=8,
                    help="how many differing samples to print")
    a = ap.parse_args()

    if not os.path.exists(a.ours):
        sys.exit("%s not found — run build/mars with --trace-pwm %s"
                 % (a.ours, a.ours))
    if not os.path.exists(a.ref):
        print("%s not found; extracting" % a.ref)
        diffsh2.extract(diffsh2.DEFAULT_ROM, diffsh2.REF_LINES)

    ref, odd, reflines = ref_stream(a.ref)
    ours = our_stream(a.ours)
    mine = ours.get("S", [])

    print("reference %s: %d slave line(s), %d sample write(s)"
          % (a.ref, reflines, len(ref)))
    if odd:
        print("  ! %d store(s) to a sample port that were not word-wide" % odd)
    for cpu, name in (("M", "master"), ("6", "68000")):
        if ours.get(cpu):
            print("  ours also has %d write(s) from the %s, which no slave log "
                  "can carry: %s" % (len(ours[cpu]), name,
                                     ", ".join("%s %04X" % w
                                               for w in ours[cpu][:4])))

    bad = 0
    for port in ("L", "R"):
        r, o = split(ref, port), split(mine, port)
        print("\n%s FIFO" % port)
        print("  reference: %s" % describe(r))
        print("  ours:      %s" % describe(o))
        n = min(len(r), len(o))
        diffs = [i for i in range(n) if r[i] != o[i]]
        if len(o) < len(r):
            print("  ! ours stops short: %d samples against %d" % (len(o), len(r)))
            bad += 1
        if not diffs:
            print("  %d of %d compared samples agree exactly" % (n, len(r)))
            continue
        bad += 1
        print("  ! %d of %d differ, first at sample %d" % (len(diffs), n, diffs[0]))
        for i in diffs[:a.rows]:
            print("      %6d  reference %04X  ours %04X" % (i, r[i], o[i]))

    if bad:
        print("\nthe 32X is not playing what the reference plays")
        return 1
    print("\nevery sample the reference's 32X played, ours played too")
    return 0


if __name__ == "__main__":
    sys.exit(main())
