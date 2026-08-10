#!/usr/bin/env python3
"""Find the first instruction where our 68000 diverges from a reference trace.

  python3 tools/diff68k.py                    diff build/trace68k.txt vs the ref
  python3 tools/diff68k.py --extract          rebuild the reference extract
  python3 tools/diff68k.py --context 30       more lines of lead-up

Our trace comes from `build/mars --trace68k`; the reference comes from an
emulator whose tracer logs one line per instruction with the register state
*before* it executes.

The reference is not a complete stream: it collapses repeats and reports each run
as "[Omitted: N]". Mirroring that policy would be the wrong move, because the
two runs' trip counts legitimately differ — the reference spins 8,277 times on a
VDP busy bit our model clears immediately — so those counts cannot say where a
line lands. Our side therefore logs everything, alignment is a monotone forward
search, and the counts are used for diagnosis: what we ran between two reference
lines against what they ran is what turns a differing trip count into a finding
instead of a lost alignment.
"""

import argparse
import os
import re
import sys
from bisect import bisect_left

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from m68k.decode import decode                       # noqa: E402

ROMS = "roms"
DEFAULT_ROM = "roms/Knuckles' Chaotix (JU) (32X) [!].32x"
REF_CACHE = "build/ref68k.txt"
OUR_TRACE = "build/trace68k.txt"
REF_LINES = 200000

FIELD = re.compile(r"^([da][0-7]|sp):([0-9a-f]{8})$")


OMITTED = re.compile(r"\[Omitted:\s*(\d+)\]")


class Rec:
    __slots__ = ("pc", "op", "regs", "sp", "flags", "text", "lineno", "gap")

    def __init__(self, pc, op, regs, sp, flags, text, lineno, gap=0):
        self.pc, self.op, self.regs = pc, op, regs
        self.sp, self.flags, self.text, self.lineno = sp, flags, text, lineno
        # Instructions the reference executed between the previous logged line
        # and this one but did not print. Always 0 in our own trace.
        self.gap = gap

    def state(self):
        return (self.pc, self.op, self.regs, self.sp, self.flags)


def parse(path, want_text=False):
    """Read a trace into records. Non-instruction lines become markers.

    Field-split rather than matched with one big regular expression: the
    register block is 17 like-shaped fields, and a pattern general enough to
    skip a variable-width mnemonic in front of them backtracks catastrophically
    on the lines that are not instructions at all.
    """
    recs, markers = [], {}
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
                    markers.setdefault(len(recs), []).append(body)
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
            recs.append(Rec(pc, op, tuple(regs), sp, tok[-1], text, n, gap))
            gap = 0
    return recs, markers


# --------------------------------------------------------------- extraction --
def find_reset_pc(rom_path):
    with open(rom_path, "rb") as f:
        head = f.read(8)
    return int.from_bytes(head[4:8], "big") & 0xFFFFFF


def extract(rom_path, out_path, limit):
    """Locate the 68000 reset in the reference logs and copy the run forward.

    The logs hold every CPU interleaved, so the 68000 is a few percent of the
    lines; and they are segmented, so the reset has to be searched for rather
    than assumed to be at the start of the first file.
    """
    pc = find_reset_pc(rom_path)
    needle = ("CPU  %06x" % pc).encode()
    logs = sorted(os.path.join(ROMS, f) for f in os.listdir(ROMS)
                  if f.endswith(".log"))
    if not logs:
        sys.exit("no reference logs in %s/" % ROMS)

    for path in logs:
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            at, chunk = -1, 1 << 24
            carry, base = b"", 0
            while True:
                buf = f.read(chunk)
                if not buf:
                    break
                hay = carry + buf
                i = hay.find(needle)
                if i >= 0:
                    at = base - len(carry) + i
                    break
                base += len(buf)
                carry = hay[-len(needle):]
            if at < 0:
                print("  %-52s no reset (%d MB)"
                      % (os.path.basename(path), size >> 20))
                continue

            print("  %s: reset PC 0x%06X at byte %d"
                  % (os.path.basename(path), pc, at))
            f.seek(at)
            n = 0
            with open(out_path, "w") as out:
                for raw in f:
                    line = raw.decode("latin1")
                    if line.startswith("CPU "):
                        out.write(line)
                        n += 1
                        if n >= limit:
                            break
            print("  wrote %s (%d lines)" % (out_path, n))
            return
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


def show(rec, dis, tag):
    r = rec.regs
    print("  %s %06x  %04x  %s" % (tag, rec.pc, rec.op, dis.at(rec.pc)))
    print("       d " + " ".join("%08x" % v for v in r[:8]))
    print("       a " + " ".join("%08x" % v for v in r[8:]))
    print("       sp:%08x  %s" % (rec.sp, rec.flags))


def report_delta(a, b):
    names = ["d%d" % i for i in range(8)] + ["a%d" % i for i in range(8)]
    out = []
    if a.pc != b.pc:
        out.append("PC %06x -> %06x" % (a.pc, b.pc))
    if a.op != b.op:
        out.append("opcode %04x -> %04x" % (a.op, b.op))
    for n, x, y in zip(names, a.regs, b.regs):
        if x != y:
            out.append("%s %08x -> %08x" % (n, x, y))
    if a.sp != b.sp:
        out.append("sp %08x -> %08x" % (a.sp, b.sp))
    if a.flags != b.flags:
        out.append("flags %s -> %s" % (a.flags, b.flags))
    return out

# -------------------------------------------------------------------- diff --
# Three ways the runs can differ, and they want different treatment.
#
# TRIPS a loop between two reference lines went round a different number of
#       times. Our count comes from the stream, theirs from the omitted run
#       length, so the difference is exact. Almost always a poll on hardware one
#       side finishes instantly and the other does not — real, but rarely fatal.
# STATE the right instruction, reached with the wrong registers. The machine is
#       already wrong yet still on the reference's path, so alignment continues
#       and only the first of a run is worth printing.
# FLOW  the reference's next instruction is one we never execute. The paths have
#       parted; the question is whether they meet again, and where.
TRIPS, STATE, FLOW = "trips", "state", "flow"


class Div:
    def __init__(self, kind, i, j):
        self.kind, self.i, self.j = kind, i, j
        self.span = 0                  # STATE: instructions until states agree
        self.ours = self.theirs = 0    # TRIPS: instructions each side ran
        self.skipped = 0               # FLOW: reference lines we never execute
        self.fatal = False

    def summary(self):
        if self.kind is TRIPS:
            n = self.ours - self.theirs
            return ("ran %d %s instruction(s) here than the reference (%d vs %d)"
                    % (abs(n), "more" if n > 0 else "fewer",
                       self.ours, self.theirs))
        if self.kind is STATE:
            return ("registers differ at the same instruction, for %d "
                    "instruction(s)" % self.span)
        if self.fatal:
            return "control flow parts and never rejoins"
        return ("control flow parts; %d reference instruction(s) we never run"
                % self.skipped)


def diff(ref, ours, window, hold, skip):
    """Align the reference against our stream and record every difference.

    Alignment is a monotone forward search: for each reference line, find the
    earliest position at or after our current one that matches it. Our position
    only ever moves forward, so a coincidental match cannot throw the walk
    thousands of instructions off course.

    The omitted counts are deliberately *not* used to predict where a line
    lands. They cannot be: the reference spins 8,277 times on a VDP busy bit
    that our model clears immediately, and a hard prediction lands 16,000
    instructions into unrelated code. They are used for diagnosis instead —
    comparing the instructions we ran between two reference lines against the
    number they ran is what turns a differing trip count into a finding.
    """
    divs = []
    i = j = agreed = 0
    run = None                          # STATE divergence being accumulated

    # Where each of our instructions sits, keyed both ways, so a realignment is
    # a lookup rather than a scan. Scanning the window is millions of state
    # comparisons per divergence, and a boot has thousands of them.
    by_state, by_pc = {}, {}
    for n, r in enumerate(ours):
        by_state.setdefault(r.state(), []).append(n)
        by_pc.setdefault((r.pc, r.op), []).append(n)

    def holds(k, at, exact):
        """Does this alignment keep working for the next few lines?

        Checked forward-only and loosely: the next reference lines must turn up
        in order, each within a short reach, which is enough to reject a
        coincidental hit without demanding the trip counts also agree.
        """
        p = at
        for d in range(1, hold):
            if k + d >= len(ref):
                return True
            nxt = find(k + d, p + 1, exact, 64)
            if nxt is None:
                return False
            p = nxt
        return True

    def find(k, frm, exact, reach):
        """Earliest position at or after `frm` matching reference line k."""
        at = (by_state if exact else by_pc).get(
            ref[k].state() if exact else (ref[k].pc, ref[k].op), ())
        p = bisect_left(at, frm)
        if p >= len(at) or at[p] > frm + reach:
            return None
        return at[p]

    def trips(i, j, ours_ran, theirs_ran):
        if ours_ran == theirs_ran:
            return
        d = Div(TRIPS, i, j)
        d.ours, d.theirs = ours_ran, theirs_ran
        divs.append(d)

    while i < len(ref) and j < len(ours):
        a = ref[i]
        di = 0
        p = j
        if (a.pc, a.op) != (ours[j].pc, ours[j].op):
            # Lexicographic in (reference lines skipped, our instructions
            # skipped): the reference's next instruction is the anchor, so look
            # for it in our stream first. Only when we never execute it at all
            # does the search start skipping reference lines — the stronger
            # claim, and what "the paths parted" actually means.
            hit = None
            for di in range(0, skip + 1):
                if i + di >= len(ref):
                    break
                for exact in (True, False):
                    p = find(i + di, j, exact, window)
                    if p is not None and holds(i + di, p, exact):
                        hit = (di, p)
                        break
                if hit:
                    break
            if hit is None:
                d = Div(FLOW, i, j)
                d.fatal = True
                divs.append(d)
                break
            di, p = hit

        if di:
            run = None
            d = Div(FLOW, i, j)
            d.skipped = di
            divs.append(d)
        else:
            # Landing where expected still leaves the run *before* the line to
            # account for: if the reference omitted instructions there and we
            # skipped a different number, one of us went round a loop the other
            # did not. Worth saying out loud rather than leaving it buried in a
            # register that happens to differ. One instruction of each side's
            # count is the logged line itself.
            trips(i, j, p - j + 1, a.gap + 1)

        # Consume ref[i + di] against ours[p], whichever way we got here, so
        # that no reference line is ever accounted for twice.
        if ref[i + di].state() == ours[p].state():
            agreed += 1
            run = None
        else:
            if run is None:
                run = Div(STATE, i + di, p)
                divs.append(run)
            run.span += 1
        i += di + 1
        j = p + 1
    return divs, agreed
# ------------------------------------------------------------------ report --
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=DEFAULT_ROM)
    ap.add_argument("--ref", default=REF_CACHE)
    ap.add_argument("--ours", default=OUR_TRACE)
    ap.add_argument("--extract", action="store_true",
                    help="rebuild the reference extract from roms/*.log")
    ap.add_argument("--limit", type=int, default=REF_LINES)
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
    ref, marks = parse(args.ref, want_text=True)
    ours, _ = parse(args.ours)
    print("reference %d instructions, ours %d" % (len(ref), len(ours)))
    if not ref or not ours:
        sys.exit("nothing to compare")

    divs, agreed = diff(ref, ours, args.window, args.hold, args.skip)
    fatal = [d for d in divs if d.fatal]
    print("%d instructions agreed; %d divergence(s), %d fatal"
          % (agreed, len(divs), len(fatal)))

    if not divs:
        print("\nthe two runs agree for the whole reference trace")
        return 0

    def our_line(j):
        return ours[j].lineno if j < len(ours) else -1

    print("\n%-4s %-9s %-9s %-8s %s"
          % ("#", "ref-line", "our-line", "ref-pc", "what"))
    n = shown = 0
    while n < len(divs):
        d = divs[n]
        # A mis-modelled poll inside a loop reports once per iteration. Collapse
        # runs that say the same thing about the same address into one row.
        same = n
        while (same + 1 < len(divs) and divs[same + 1].kind is d.kind
               and ref[divs[same + 1].i].pc == ref[d.i].pc
               and divs[same + 1].summary() == d.summary()):
            same += 1
        print("%-4d %-9d %-9d %-8s %s%s"
              % (n + 1, ref[d.i].lineno, our_line(d.j), "%06x" % ref[d.i].pc,
                 d.summary(),
                 "   [x%d, same address]" % (same - n + 1) if same > n else ""))
        shown += 1
        n = same + 1
        if shown >= args.rows and n < len(divs):
            print("... %d more rows; raise --rows to see them"
                  % (len(divs) - n))
            break

    # Detail: the fatal divergence, plus the first few others for context.
    detail = fatal[:1] + [d for d in divs if not d.fatal][:args.detail]
    detail.sort(key=lambda d: d.i)
    for d in detail:
        i, j = d.i, d.j
        ran_out = j >= len(ours)
        head = ("OUR TRACE ENDS HERE — nothing left to compare" if ran_out
                else "FATAL DIVERGENCE — control flow never rejoins" if d.fatal
                else "divergence — " + d.summary())
        print("\n" + "=" * 74)
        print("%s\n  reference line %d, our line %d"
              % (head, ref[i].lineno, our_line(j)))
        print("=" * 74)
        for k in range(max(0, i - args.context), i):
            for m in marks.get(k, []):
                print("       ! %s" % m)
            if ref[k].gap:
                print("       . %d instruction(s) the reference did not print"
                      % ref[k].gap)
            print("    %06x  %04x  %s"
                  % (ref[k].pc, ref[k].op, ref[k].text or dis.at(ref[k].pc)))
        for m in marks.get(i, []):
            print("       ! reference took an interrupt here: %s" % m)
        if ref[i].gap:
            print("       . %d instruction(s) the reference did not print"
                  % ref[i].gap)
        show(ref[i], dis, "ref")
        if ran_out:
            continue
        show(ours[j], dis, "our")
        print("\n  delta ref -> ours:")
        for x in report_delta(ref[i], ours[j]) or ["(none)"]:
            print("    %s" % x)
        print("\n  what each side does next:")
        print("    ref %s" % " -> ".join("%06x" % r.pc for r in ref[i:i + 10]))
        print("    our %s" % " -> ".join("%06x" % r.pc for r in ours[j:j + 10]))
    return 1 if fatal else 0


if __name__ == "__main__":
    sys.exit(main())
