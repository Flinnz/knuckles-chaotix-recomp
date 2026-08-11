"""Align one CPU trace against a reference trace and report where they part.

This is the part of `tools/diff68k.py` that turned out not to be about the
68000 at all: the alignment, the classification of a difference into one of
three kinds, and the reporting. `tools/diffsh2.py` needs exactly the same thing
for the SH-2s, where the records hold different registers and a line means a
basic block rather than an instruction.

What a caller supplies is a `Cpu`: how to render a record, how to describe the
delta between two, and how wide a program counter is. Everything else here is
common.

The reference is not a complete stream. It collapses repeats and reports each
run as "[Omitted: N]". Mirroring that policy would be the wrong move, because
the two runs' trip counts legitimately differ — the reference spins 8,277 times
on a VDP busy bit our model clears immediately — so those counts cannot say
where a line lands. Our side therefore logs everything, alignment is a monotone
forward search, and the counts are used for diagnosis: what we ran between two
reference lines against what they ran is what turns a differing trip count into
a finding instead of a lost alignment.
"""

from bisect import bisect_left


class Rec:
    """One logged step: an instruction for the 68000, a block for the SH-2.

    `ident` is whatever else pins the step down beyond its address — the opcode
    word, where the log carries one. `regs` is every register in the order the
    Cpu names them, so nothing here has to know which is which.
    """

    __slots__ = ("pc", "ident", "regs", "flags", "text", "lineno", "gap", "ran")

    def __init__(self, pc, ident, regs, flags, text, lineno, gap=0, ran=1):
        self.pc, self.ident, self.regs = pc, ident, regs
        self.flags, self.text, self.lineno = flags, text, lineno
        # Instructions the reference executed between the previous logged line
        # and this one without logging them: collapsed repeats, and — for the
        # SH-2, where a line is a block — the rest of the block it was in.
        self.gap = gap
        # Instructions this record accounts for before the next one. One per
        # line for an instruction trace; a whole basic block for the SH-2's, so
        # that a trip count means the same thing on both sides.
        self.ran = ran

    def state(self):
        return (self.pc, self.ident, self.regs, self.flags)

    def flow(self):
        return (self.pc, self.ident)


class Cpu:
    """What differs between the two machines: naming, rendering, PC width."""

    def __init__(self, names, pcw, annotate, show_regs):
        self.names = names            # register names, in `Rec.regs` order
        self.pcw = pcw                # hex digits in a program counter
        self.annotate = annotate      # addr -> disassembly text
        self.show_regs = show_regs    # Rec -> lines of register display

    def addr(self, a):
        return "%0*x" % (self.pcw, a)

    def ident(self, rec):
        """The opcode word, where the log carries one. Blank when it does not."""
        return "" if rec.ident is None else "%04x  " % rec.ident

    def line(self, rec):
        """One record as address, opcode and text — the reference's own text
        where it has one, ours where it does not."""
        return "%s  %s%s" % (self.addr(rec.pc), self.ident(rec),
                             rec.text or self.annotate(rec.pc))

    def show(self, rec, tag):
        # Deliberately our decoder rather than `rec.text`: both sides of a
        # divergence are then annotated the same way, so a disagreement between
        # the two disassemblers cannot be mistaken for one between the runs.
        out = ["  %s %s  %s%s" % (tag, self.addr(rec.pc), self.ident(rec),
                                  self.annotate(rec.pc))]
        return out + self.show_regs(rec)

    def delta(self, a, b):
        """Every field that differs, reference first."""
        out = []
        if a.pc != b.pc:
            out.append("PC %s -> %s" % (self.addr(a.pc), self.addr(b.pc)))
        if a.ident != b.ident and a.ident is not None and b.ident is not None:
            out.append("opcode %04x -> %04x" % (a.ident, b.ident))
        for n, x, y in zip(self.names, a.regs, b.regs):
            if x != y:
                out.append("%s %08x -> %08x" % (n, x, y))
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
# STATE the right step, reached with the wrong registers. The machine is
#       already wrong yet still on the reference's path, so alignment continues
#       and only the first of a run is worth printing.
# FLOW  the reference's next step is one we never take. The paths have parted;
#       the question is whether they meet again, and where.
TRIPS, STATE, FLOW = "trips", "state", "flow"


class Div:
    def __init__(self, kind, i, j):
        self.kind, self.i, self.j = kind, i, j
        self.span = 0                  # STATE: steps until states agree
        self.ours = self.theirs = 0    # TRIPS: steps each side ran
        self.skipped = 0               # FLOW: reference lines we never execute
        self.exhausted = False         # FLOW: our stream simply ran out
        self.fatal = False

    def summary(self, unit="instruction", ran_unit=None):
        if self.kind is TRIPS:
            n = self.ours - self.theirs
            return ("ran %d %s %s(s) here than the reference (%d vs %d)"
                    % (abs(n), "more" if n > 0 else "fewer", ran_unit or unit,
                       self.ours, self.theirs))
        if self.kind is STATE:
            return ("registers differ at the same %s, for %d %s(s)"
                    % (unit, self.span, unit))
        if self.exhausted:
            return "our trace ends here — inconclusive, run more frames"
        if self.fatal:
            return "control flow parts and never rejoins"
        return ("control flow parts; %d reference %s(s) we never run"
                % (self.skipped, unit))


def diff(ref, ours, window, hold, skip):
    """Align the reference against our stream and record every difference.

    Alignment is a monotone forward search: for each reference line, find the
    earliest position at or after our current one that matches it. Our position
    only ever moves forward, so a coincidental match cannot throw the walk
    thousands of steps off course.

    The omitted counts are deliberately *not* used to predict where a line
    lands. They cannot be: the reference spins 8,277 times on a VDP busy bit
    that our model clears immediately, and a hard prediction lands 16,000
    instructions into unrelated code. They are used for diagnosis instead —
    comparing what we ran between two reference lines against what they ran is
    what turns a differing trip count into a finding.
    """
    divs = []
    i = j = agreed = 0
    run = None                          # STATE divergence being accumulated

    # Where each of our steps sits, keyed both ways, so a realignment is a
    # lookup rather than a scan. Scanning the window is millions of state
    # comparisons per divergence, and a boot has thousands of them.
    by_state, by_pc = {}, {}
    for n, r in enumerate(ours):
        by_state.setdefault(r.state(), []).append(n)
        by_pc.setdefault(r.flow(), []).append(n)

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
            ref[k].state() if exact else ref[k].flow(), ())
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
        if a.flow() != ours[j].flow():
            # Lexicographic in (reference lines skipped, our steps skipped): the
            # reference's next step is the anchor, so look for it in our stream
            # first. Only when we never take it at all does the search start
            # skipping reference lines — the stronger claim, and what "the paths
            # parted" actually means.
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
                # Our stream ending is not the paths parting. If there is less
                # of it left than the search needs, the answer is "run longer",
                # not "diverged" — and a long copy loop is easily that big.
                d.exhausted = len(ours) - j < window
                d.fatal = not d.exhausted
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
            # account for: if the reference omitted steps there and we skipped a
            # different number, one of us went round a loop the other did not.
            # Worth saying out loud rather than leaving it buried in a register
            # that happens to differ.
            #
            # Both counts are instructions, and both measure the same half-open
            # interval — from the previously matched record up to this one, the
            # earlier record's own steps included and this one's excluded.
            # Theirs is `gap + 1` because `gap` is what they ran between the two
            # lines without printing it; ours is `ran` summed from `j - 1`, the
            # record that matched the previous reference line. For an
            # instruction trace every `ran` is 1 and this is just "how many
            # lines apart"; for a block trace it is the block lengths, a block
            # always executing whole.
            #
            # This is exact only if our tracer never logs a block it did not
            # run. It used to: SH2_BLOCK and M68K_BLOCK traced before checking
            # the fuel, so a block that yielded was logged, re-entered next
            # slice and logged again. The two records are identical, so the
            # alignment could not see it, and every count here came out about
            # double.
            if j:
                trips(i, j, sum(r.ran for r in ours[j - 1:p]), a.gap + 1)

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

    # Our stream running out while everything still matched is not agreement,
    # it is a short run — and the walk above just ends when it happens, with
    # nothing recorded. Say so, or a truncated trace reads as a clean pass.
    if i < len(ref) and j >= len(ours):
        d = Div(FLOW, i, len(ours))
        d.exhausted = True
        divs.append(d)
    return divs, agreed


# ------------------------------------------------------------------ report --
def report(cpu, ref, ours, marks, divs, agreed, args, unit="instruction",
           ran_unit=None):
    """Print the summary table and then the divergences worth reading."""
    fatal = [d for d in divs if d.fatal]
    stop = divs[-1] if divs and (divs[-1].fatal or divs[-1].exhausted) else None
    print("%d %ss agreed; %d divergence(s), %d fatal"
          % (agreed, unit, len(divs), len(fatal)))
    print("reached reference line %d of %d%s"
          % (ref[stop.i].lineno if stop else ref[-1].lineno, ref[-1].lineno,
             "" if stop else " — the whole extract"))

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
               and divs[same + 1].summary(unit, ran_unit)
                   == d.summary(unit, ran_unit)):
            same += 1
        print("%-4d %-9d %-9d %-8s %s%s"
              % (n + 1, ref[d.i].lineno, our_line(d.j), cpu.addr(ref[d.i].pc),
                 d.summary(unit, ran_unit),
                 "   [x%d, same address]" % (same - n + 1) if same > n else ""))
        shown += 1
        n = same + 1
        if shown >= args.rows and n < len(divs):
            print("... %d more rows; raise --rows to see them"
                  % (len(divs) - n))
            break

    # Detail: the fatal divergence, plus the first few others for context — or,
    # when an address is named, that address's own rows however far down the
    # list they are. A summary row says a group exists; only the full record
    # says what it is, and the group worth reading is rarely the first one.
    at = getattr(args, "detail_at", None)
    if at:
        detail = [d for d in divs if ref[d.i].pc in at][:max(args.detail, 1)]
    else:
        detail = [d for d in divs if d.fatal or d.exhausted][:1]
        detail += [d for d in divs if not (d.fatal or d.exhausted)][:args.detail]
    detail.sort(key=lambda d: d.i)
    for d in detail:
        i, j = d.i, d.j
        ran_out = j >= len(ours)
        head = ("OUR TRACE ENDS HERE — nothing left to compare" if ran_out
                else "INCONCLUSIVE — our trace ends here; run more frames"
                if d.exhausted
                else "FATAL DIVERGENCE — control flow never rejoins" if d.fatal
                else "divergence — " + d.summary(unit, ran_unit))
        print("\n" + "=" * 74)
        print("%s\n  reference line %d, our line %d"
              % (head, ref[i].lineno, our_line(j)))
        print("=" * 74)
        for k in range(max(0, i - args.context), i):
            for m in marks.get(ref[k].lineno, []):
                print("       ! %s" % m)
            if ref[k].gap:
                print("       . %d %s(s) the reference did not print"
                      % (ref[k].gap, ran_unit or unit))
            print("    %s" % cpu.line(ref[k]))
        for m in marks.get(ref[i].lineno, []):
            print("       ! reference took an interrupt here: %s" % m)
        if ref[i].gap:
            print("       . %d %s(s) the reference did not print"
                  % (ref[i].gap, ran_unit or unit))
        for l in cpu.show(ref[i], "ref"):
            print(l)
        if ran_out:
            continue
        for l in cpu.show(ours[j], "our"):
            print(l)
        print("\n  delta ref -> ours:")
        for x in cpu.delta(ref[i], ours[j]) or ["(none)"]:
            print("    %s" % x)
        print("\n  what each side does next:")
        print("    ref %s" % " -> ".join(cpu.addr(r.pc) for r in ref[i:i + 10]))
        print("    our %s" % " -> ".join(cpu.addr(r.pc) for r in ours[j:j + 10]))
    # Stopping short is not a pass either. `exhausted` means our own trace ran
    # out before the reference did, which is what a crashed or truncated run
    # looks like — and a gate that called that success would sleep through
    # exactly the failure it exists to catch.
    return 1 if stop else 0


# --------------------------------------------------------------- log search --
def find_in(path, needle):
    """Byte offset of `needle` in a file too large to hold in memory."""
    with open(path, "rb") as f:
        carry, base, chunk = b"", 0, 1 << 24
        while True:
            buf = f.read(chunk)
            if not buf:
                return -1
            hay = carry + buf
            i = hay.find(needle)
            if i >= 0:
                return base - len(carry) + i
            base += len(buf)
            carry = hay[-len(needle):]
