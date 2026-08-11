#!/usr/bin/env python3
"""What a 68000 loop costs in the reference, in cycles an instruction.

  python3 tools/refpoll.py                the hot spin loops of the first 14 frames
  python3 tools/refpoll.py --frames 40    further into the steady state
  python3 tools/refpoll.py --rate         68000 instructions a frame, by phase
  python3 tools/refpoll.py --pc 0x8818AA  one loop, run by run

This was written to find what a 68000 read of a 32X register costs over a read of
RAM, on the belief that the answer was six or seven cycles and that not charging
it was why the engine reached the master's first command 2.5 frames early. The
answer is **nothing**: across six loops it found on its own, three reading work
RAM and three reading a 32X register, every one comes out within 0.4% of what
the manual charges, and the RAM loops are the control that says the clock is
sound. The six or seven cycles were an artefact of the clock the first
measurement used, which is the thing this had to get right and is most of what
is written below.

What it is still good for is what it did next: pricing any loop the 68000 sits
in, against a clock that holds through the boot, which is how the real residue
turned out to be a flat conversion constant on our side rather than a wait state
on the machine's.

A price is elapsed cycles over instructions retired, so it needs a clock, and the
whole difficulty is that the phase where the answer is wrong is the phase with
the fewest clocks in it. Three candidates are in the log and two of them do not
survive contact with the boot:

* The 68000's *taken* vertical interrupt is exactly 127,840 cycles from the next
  — but only once the engine unmasks it. Through the boot the 68000 runs with
  interrupts off and 937,053 instructions go by without a single marker.
* The VDP's own `vblank` line is logged whether or not the CPU takes it, which
  sounds like the clock this wants, and is not: 72 of them arrive in bursts with
  no instructions in between.
* The slave's **PWM interrupt** is a hardware timer with a fixed period, it does
  not care what any CPU is doing, and it ticks about 367 times a frame — fine
  enough to time a run of a few thousand instructions. It only stops when the
  slave does, which is why this reports the tick count alongside every figure it
  derives from it: a span with no ticks in it has not been timed at all.

So the PWM tick is the clock, and the steady frames calibrate it — the phase
where the taken interrupt is available is exactly the phase where it can say what
a tick is worth. The master's instruction count is reported next to it as a
second opinion, because that is the clock the first pass at this used, and a
figure that only one of the two believes is a figure to distrust.

A "loop" here is discovered rather than named in advance: a maximal run of
consecutive 68000 instructions that stays inside a 64-byte window and revisits a
PC — which finds the polls without anyone having to know where they are. The
log's own disassembly says what each reads, and that is the whole question,
because what separates the two phases is whether the 68000 is polling a 32X
register across the adapter or a flag in its own work RAM.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import diff68k                                          # noqa: E402
from recompile68k import CYCLES_PATH, read_cycles        # noqa: E402

DEFAULT_ROM = "roms/Knuckles' Chaotix (JU) (32X) [!].32x"
OMITTED = re.compile(rb"\[Omitted:\s*(\d+)\]")

# Musashi's 68000 costs, the same table the recompiled build charges from. What
# it buys here is a loop's price without a clock: a lap of a closed loop costs
# what its instructions cost, so counting laps against PWM ticks measures the
# *tick* instead of assuming it.
CYCLES = read_cycles(CYCLES_PATH)

# Mirrors CYCLES_PER_FRAME in src/mars_main.c: the Genesis master clock over
# seven, across a frame of 262 lines of 3,420 master clocks — 59.9227 Hz, not
# 60. The SH-2s are clocked at exactly three times the 68000.
#
# It was 127,840 here too, and every loop this tool prices came out uniformly
# 0.2% under what the manual charges — 10.98 for a pair costing 11, 12.97 for a
# pair costing 13. A systematic offset on every loop at once is a clock, not six
# coincidences. Corrected they read 11.00 and 13.00.
CYCLES_PER_FRAME = 128006
SH2_MULTIPLIER = 3

# Which taken-vblank frames calibrate the PWM tick. The first two are the tail
# of the boot and the frame the engine unmasks in; from the third on the rate is
# 367 to a frame and does not move.
CAL_FROM = 3

# A run has to stay this close together to still be the same loop, and be this
# long to be worth pricing. The window is deliberately tight: a poll is two or
# three instructions and a `dbra` fill is not much longer, where a call into a
# decompressor leaves it immediately.
WINDOW = 64
MIN_INSNS = 200

# The bucket `--rate` reports in, in PWM ticks: about one frame. A bucket that
# spans a stretch where the slave was halted has not been timed — no ticks
# arrive, the bucket stays open, and the absurd instruction count it ends up
# with is the signal that it should be read as "the clock stopped here" rather
# than as a rate.
RATE_TICKS = 367


class Loop:
    """One discovered spin site, with everything needed to price it."""

    def __init__(self, lo):
        self.lo = lo
        self.hi = lo
        self.insns = 0          # 68000 instructions retired inside it
        self.master = 0         # master instructions elapsed over the same span
        self.pwm = 0            # PWM ticks elapsed over the same span
        self.runs = 0
        self.first_frame = None
        self.last_frame = None
        self.text = {}          # pc -> the reference's own disassembly
        self.op = {}            # pc -> its opcode word, for the cycle table

    def add(self, hi, insns, master, pwm, frame, text, op):
        self.op.update(op)
        self.hi = max(self.hi, hi)
        self.insns += insns
        self.master += master
        self.pwm += pwm
        self.runs += 1
        if self.first_frame is None:
            self.first_frame = frame
        self.last_frame = frame
        self.text.update(text)

    def body(self, width=3):
        return " ; ".join(self.text[a] for a in sorted(self.text)[:width])

    def cycle(self):
        """The instructions that actually go round, and what one lap costs.

        Only for a loop that closes on itself: the last thing in it branches to
        the first thing in it. Then every PC between the two runs once a lap and
        the branch is taken every lap but the last, so a `bcc`'s table entry —
        which is its taken cost — is exactly right with nothing to adjust.
        Anything after the branch is the exit and is not in the lap.

        A `dbcc` is the exception and has to be paid its CYC_DBCC_F_NOEXP: its
        entry is 12, which is neither of the prices it actually goes for, and
        looping costs 10. Left alone it puts a five-instruction fill loop 3.4%
        above the polls, which is what caught it.

        Returns (instructions in a lap, cycles in a lap), or None when the loop
        does not close — which is most of them, including any whose head the
        reference collapsed away into an `[Omitted]` run.
        """
        pcs = sorted(self.text)
        if not pcs:
            return None
        for i, pc in enumerate(pcs):
            m = re.search(r"\$([0-9a-f]{4,6})\s*$", self.text[pc])
            if m and int(m.group(1), 16) == pcs[0]:
                lap = pcs[:i + 1]
                if any(a not in self.op for a in lap):
                    return None
                cost = sum(CYCLES[self.op[a]] for a in lap)
                if self.text[pc].split()[1].startswith("db"):
                    cost -= 2
                return len(lap), cost
        return None

    def crosses(self):
        """Does any instruction in it reach across the adapter?

        The 32X's registers appear to the 68000 at 0xA15100-0xA153FF and its
        frame buffer through the window at 0x840000; everything else it touches
        is the Genesis side, which it owns outright.
        """
        for t in self.text.values():
            for m in re.finditer(r"\$([0-9a-f]{6})", t):
                a = int(m.group(1), 16)
                if 0xA15100 <= a < 0xA15400 or 0x840000 <= a < 0x880000:
                    return True
        return False


def scan(rom_path, want_frames, only_pc=None):
    """Walk the logs from the console reset, discovering and timing loops."""
    first, at, logs = diff68k.find_reset(diff68k.find_reset_pc(rom_path))

    loops, runs_of, frames, rate = {}, [], [], []
    cpu_n = master_n = pwm_n = 0
    frame = 0
    next_mark = RATE_TICKS

    # The open run: where it started on both clocks, and what it has covered.
    r_lo = r_hi = None
    r_insns = 0
    r_master0 = r_pwm0 = 0
    r_text = {}
    r_op = {}
    r_seen = set()
    r_repeat = False

    def close():
        nonlocal r_lo, r_hi, r_insns, r_text, r_op, r_seen, r_repeat
        if r_lo is not None and r_repeat and r_insns >= MIN_INSNS:
            if only_pc is None or r_lo == only_pc:
                lp = loops.setdefault(r_lo, Loop(r_lo))
                lp.add(r_hi, r_insns, master_n - r_master0, pwm_n - r_pwm0,
                       frame, r_text, r_op)
                if only_pc is not None:
                    runs_of.append((frame, r_insns, master_n - r_master0,
                                    pwm_n - r_pwm0))
        r_lo = r_hi = None
        r_insns = 0
        r_text = {}
        r_op = {}
        r_seen = set()
        r_repeat = False

    for k in range(first, len(logs)):
        with open(logs[k], "rb") as f:
            if k == first:
                f.seek(at)
            for raw in f:
                tag = raw[:3]
                if tag == b"SHM":
                    body = raw.split(b" Instruction: ", 1)[-1]
                    m = OMITTED.search(body)
                    master_n += int(m.group(1)) if m else 1
                    continue
                if tag == b"SHS":
                    if b"Interrupt: PWM" in raw:
                        pwm_n += 1
                        if pwm_n >= next_mark:
                            rate.append((pwm_n, cpu_n, master_n))
                            next_mark += RATE_TICKS
                    continue
                if tag != b"CPU":
                    continue

                body = raw.split(b" Instruction: ", 1)[-1]
                m = OMITTED.search(body)
                if m:
                    # A collapsed run is instructions the 68000 really ran, at
                    # the PC of the line before it — which is exactly the poll
                    # this is here to price, so dropping them drops the
                    # measurement itself.
                    n = int(m.group(1))
                    cpu_n += n
                    if r_lo is not None:
                        r_insns += n
                        r_repeat = True
                    continue

                if b"Interrupt: Vblank" in body:
                    frame += 1
                    frames.append((cpu_n, master_n, pwm_n))
                    if frame >= want_frames:
                        close()
                        return (loops, runs_of, frames, rate,
                                cpu_n, master_n, pwm_n)
                    continue

                tok = body.split(maxsplit=3)
                if len(tok) < 3 or tok[0] != b"CPU":
                    continue
                try:
                    pc = int(tok[1], 16)
                except ValueError:
                    continue
                cpu_n += 1

                if r_lo is not None and (pc < r_hi - WINDOW or pc > r_lo + WINDOW):
                    close()
                if r_lo is None:
                    r_lo = r_hi = pc
                    r_master0, r_pwm0 = master_n, pwm_n
                r_lo, r_hi = min(r_lo, pc), max(r_hi, pc)
                if pc in r_seen:
                    r_repeat = True
                r_seen.add(pc)
                r_insns += 1
                if pc not in r_text and len(r_text) < 24:
                    rest = tok[3].split(b" d0:", 1)[0] if len(tok) > 3 else b""
                    r_text[pc] = "%06X %s" % (
                        pc, " ".join(rest.decode(errors="replace").split()))
                    try:
                        r_op[pc] = int(tok[2], 16)
                    except ValueError:
                        pass

    close()
    return loops, runs_of, frames, rate, cpu_n, master_n, pwm_n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=DEFAULT_ROM)
    ap.add_argument("--frames", type=int, default=14,
                    help="stop after this many *taken* vertical interrupts")
    ap.add_argument("--pc", type=lambda s: int(s, 0), default=None,
                    help="one loop head, reported run by run")
    ap.add_argument("--rows", type=int, default=16)
    ap.add_argument("--width", type=int, default=3,
                    help="instructions of each loop body to print")
    ap.add_argument("--rate", action="store_true",
                    help="68000 instructions a frame, on the PWM clock")
    ap.add_argument("--period", action="store_true",
                    help="what a PWM tick is worth, from the closed loops")
    a = ap.parse_args()

    loops, runs, frames, rate, cpu_n, master_n, pwm_n = scan(
        a.rom, a.frames, a.pc)
    if len(frames) <= CAL_FROM:
        sys.exit("need more than %d taken vertical interrupts to calibrate"
                 % CAL_FROM)

    # The calibration. Between two taken vertical interrupts is exactly one
    # frame, so the PWM ticks counted across the steady ones say what a tick is
    # worth — and the same frames say it about a master instruction, which is
    # the second opinion every figure below is checked against.
    span = frames[-1][0] - frames[CAL_FROM][0], \
        frames[-1][1] - frames[CAL_FROM][1], \
        frames[-1][2] - frames[CAL_FROM][2]
    nf = len(frames) - 1 - CAL_FROM
    cyc_per_pwm = nf * CYCLES_PER_FRAME / span[2] if span[2] else 0.0
    master_cpi = nf * CYCLES_PER_FRAME * SH2_MULTIPLIER / span[1] if span[1] else 0.0
    print("\n  calibrated over %d steady frames: %.1f PWM ticks a frame, "
          "%.1f 68000 cycles each" % (nf, span[2] / nf, cyc_per_pwm))
    print("  the same frames put the master at %.3f cycles an instruction, "
          "and the 68000 at %.0f instructions a frame"
          % (master_cpi, span[0] / nf))
    print("  the whole scan: %d 68000, %d master instructions, %d PWM ticks"
          % (cpu_n, master_n, pwm_n))

    def by_pwm(pwm, insns):
        return pwm * cyc_per_pwm / insns if insns else 0.0

    def by_master(master, insns):
        return master * master_cpi / SH2_MULTIPLIER / insns if insns else 0.0

    if a.period:
        # The PWM tick, measured instead of assumed — and this is the one
        # measurement here that needs no clock at all going in. A closed loop
        # costs what its instructions cost, so its laps *are* a clock, and
        # counting PWM ticks against them says what a tick is worth. The SH-2
        # runs at three times the 68000, so three times that is the timer's
        # period, which is the number `mars_pwm_period()` computes from the
        # registers the slave programmed.
        print("\n  what a PWM tick is worth, from loops that close on"
              " themselves:")
        print("  %-15s %8s %10s %7s %10s %9s"
              % ("loop", "lap", "insns", "pwm", "cyc/tick", "period"))
        rows = []
        for lp in sorted(loops.values(), key=lambda l: -l.insns):
            cyc = lp.cycle()
            if not cyc or not lp.pwm or lp.insns < 4000:
                continue
            n, cost = cyc
            per_tick = lp.insns * cost / n / lp.pwm
            rows.append(per_tick)
            print("  %06X-%06X %3d/%-4d %10d %7d %10.2f %9.1f"
                  % (lp.lo, lp.hi, n, cost, lp.insns, lp.pwm, per_tick,
                     per_tick * SH2_MULTIPLIER))
        if rows:
            mean = sum(rows) / len(rows)
            print("  %d loop(s): %.2f 68000 cycles a tick, **%.1f SH-2 cycles**"
                  % (len(rows), mean, mean * SH2_MULTIPLIER))

    if a.rate:
        # What the boot phase actually runs at. The claim this replaces —
        # 7,808 instructions a frame against our 9,605 — came of timing the
        # same window by the master's instruction count at its *steady-state*
        # rate, and the master does not run at that rate here.
        print("\n  68000 instructions a frame, timed by the PWM tick:")
        print("  %-8s %10s %10s %9s %12s %8s"
              % ("frame", "insns", "a frame", "cyc/insn", "master", "shm c/i"))
        p0 = c0 = m0 = 0
        for i, (p, c, m) in enumerate(rate, 1):
            dp, dc, dm = p - p0, c - c0, m - m0
            cyc = dp * cyc_per_pwm
            print("  %-8d %10d %10.0f %9.2f %12d %8.3f"
                  % (i, dc, dc * RATE_TICKS / dp, cyc / dc if dc else 0.0, dm,
                     cyc * SH2_MULTIPLIER / dm if dm else 0.0))
            p0, c0, m0 = p, c, m

    if a.pc is not None:
        print("\n  runs of the loop at 0x%06X:" % a.pc)
        print("  %-7s %10s %8s %12s %9s %9s"
              % ("frame", "insns", "pwm", "master", "c/i pwm", "c/i shm"))
        for fr, insns, master, pwm in runs[:a.rows]:
            print("  %-7d %10d %8d %12d %9.2f %9.2f"
                  % (fr, insns, pwm, master, by_pwm(pwm, insns),
                     by_master(master, insns)))

    print("\n  the 68000's hot loops, dearest first. `A` marks a loop that"
          " reaches across the adapter:")
    print("  %-15s %-7s %10s %7s %8s %8s  %s"
          % ("where", "frames", "insns", "pwm", "c/i pwm", "c/i shm", "body"))
    order = sorted(loops.values(), key=lambda l: -l.insns)
    for lp in order[:a.rows]:
        print("  %06X-%06X %2d-%-4d %10d %7d %8.2f %8.2f %s %s"
              % (lp.lo, lp.hi, lp.first_frame, lp.last_frame, lp.insns, lp.pwm,
                 by_pwm(lp.pwm, lp.insns), by_master(lp.master, lp.insns),
                 "A" if lp.crosses() else " ", lp.body(a.width)))


if __name__ == "__main__":
    sys.exit(main())
