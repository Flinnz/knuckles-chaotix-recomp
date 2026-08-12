#!/usr/bin/env python3
"""Check the PSG against arithmetic, since the game never lets it be heard.

This game's driver attenuates all four channels to 15 in its init and never
lifts one in anything the reference logs contain, so the trace comparison —
which is what every other core here is held to — says nothing at all about
whether this one works. What can be checked instead is that the chip's own
numbers come out: a tone channel's frequency is clock / (32 * period), an
attenuation step is two decibels, and periodic noise repeats every sixteen
shifts of its register. None of those is a table copied out of this file's
source; each is computed here and compared against what the core does.

The harness is generated, compiled against src/psg.c and run, the same shape as
the two recompiler tests.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from hostcc import EXE, host_cc                  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "test")
CLOCK = 3579545          # the Z80's, which is the PSG's

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "psg.h"

static PSG p;
static int fails;

static void ok(const char *what, double got, double want, double tol) {
    int good = fabs(got - want) <= tol;
    if (!good) fails++;
    printf("  %-4s  %-52s got %-12.4f want %.4f\n",
           good ? "PASS" : "FAIL", what, got, want);
}

/* Count the output transitions of channel `ch` over `ticks` PSG clocks, from a
 * settled start: the first tick after a reset moves a parked channel to its
 * held level, which is a state change and not an oscillation. */
static long transitions(int ch, long ticks) {
    long n = 0;
    psg_tick(&p, 16);
    int last = p.flip[ch];
    for (long i = 16; i < ticks; i += 16) {
        psg_tick(&p, 16);
        if (p.flip[ch] != last) { n++; last = p.flip[ch]; }
    }
    return n;
}

static void set_tone(int ch, unsigned period, unsigned att) {
    psg_write(&p, (uint8_t)(0x80 | (ch << 5) | (period & 0x0F)));
    psg_write(&p, (uint8_t)((period >> 4) & 0x3F));
    psg_write(&p, (uint8_t)(0x90 | (ch << 5) | (att & 0x0F)));
}

int main(void) {
    /* --- the register decode, which is the one thing the game does exercise */
    psg_reset(&p);
    psg_write(&p, 0x8D); psg_write(&p, 0x11);
    ok("latch 8D then data 11 is channel 0 period 0x11D",
       p.period[0], 0x11D, 0);
    psg_write(&p, 0xA7); psg_write(&p, 0x1A);
    ok("latch A7 then data 1A is channel 1 period 0x1A7",
       p.period[1], 0x1A7, 0);
    psg_write(&p, 0x9F);
    ok("9F is channel 0 attenuation 15", p.vol[0], 15, 0);
    psg_write(&p, 0x05);
    ok("a data byte after a volume latch keeps the four low bits",
       p.vol[0], 5, 0);
    psg_write(&p, 0xE4);
    ok("E4 is the noise control, white at the middle rate",
       p.period[3], 4, 0);

    /* --- a tone channel's frequency is clock / (32 * period) --------------- */
    unsigned periods[] = { 1, 285, 0x3FF };
    for (unsigned i = 0; i < sizeof periods / sizeof *periods; i++) {
        unsigned per = periods[i];
        psg_reset(&p);
        set_tone(0, per, 0);
        long ticks = 16L * 2 * per * 200;              /* 200 whole cycles */
        long n = transitions(0, ticks);
        double hz = (double)n / 2.0 / ((double)ticks / @CLOCK@);
        char what[80];
        snprintf(what, sizeof what, "period %u is %.1f Hz", per,
                 @CLOCK@ / (32.0 * per));
        ok(what, hz, @CLOCK@ / (32.0 * per), @CLOCK@ / (32.0 * per) * 0.01);
    }

    /* A period of zero is a parked channel, not an oscillator. */
    psg_reset(&p);
    set_tone(0, 0, 0);
    ok("period 0 never toggles", (double)transitions(0, 16L * 4096), 0, 0);

    /* --- two decibels an attenuation step --------------------------------- */
    psg_reset(&p);
    double prev = 0;
    int steps_ok = 1;
    for (unsigned att = 0; att < 15; att++) {
        set_tone(0, 1, att);
        psg_tick(&p, 16 * 4096);
        double lv = psg_level(&p);
        if (att) {
            double ratio = lv / prev;
            if (fabs(ratio - pow(10.0, -0.1)) > 0.02) steps_ok = 0;
        }
        prev = lv;
    }
    ok("every attenuation step is a factor of 10^-0.1", steps_ok, 1, 0);
    set_tone(0, 1, 15);
    psg_tick(&p, 16 * 4096);
    ok("attenuation 15 is silence", psg_level(&p), 0, 0);

    /* --- the noise register ----------------------------------------------- */
    psg_reset(&p);
    psg_write(&p, 0xE0);                    /* periodic, the fastest rate */
    psg_write(&p, 0x9F); psg_write(&p, 0xBF); psg_write(&p, 0xDF);
    psg_write(&p, 0xF0);                    /* channel 3 at full volume */
    {
        /* Periodic noise is the shift register cycling, so its output repeats
         * once every sixteen shifts and the rate register says how long a
         * shift is: 0x10 counter reloads, two to a cycle, sixteen clocks each. */
        long shift = 16L * 0x10 * 2;
        int first[16], again[16];
        for (int i = 0; i < 16; i++) { psg_tick(&p, shift); first[i] = p.lfsr & 1; }
        for (int i = 0; i < 16; i++) { psg_tick(&p, shift); again[i] = p.lfsr & 1; }
        int same = 1, ones = 0;
        for (int i = 0; i < 16; i++) { same &= first[i] == again[i]; ones += first[i]; }
        ok("periodic noise repeats every 16 shifts", same, 1, 0);
        ok("and is one bit high in sixteen", ones, 1, 0);
    }
    psg_reset(&p);
    psg_write(&p, 0xE4);                    /* white, the same rate */
    {
        long shift = 16L * 0x10 * 2;
        uint16_t start = p.lfsr;
        int repeated = 0;
        for (int i = 0; i < 4096; i++) {
            psg_tick(&p, shift);
            if (p.lfsr == start) { repeated = 1; break; }
        }
        ok("white noise does not repeat within 4096 shifts", repeated, 0, 0);
    }

    /* The three fixed rates are the clock over 512, 1024 and 2048, and the
     * fourth borrows channel 2's period so a driver can sweep the noise. Both
     * are measured as rates: a phase test would only be asserting where the
     * counter happened to be when the register was written. */
    for (unsigned rate = 0; rate < 3; rate++) {
        psg_reset(&p);
        psg_write(&p, (uint8_t)(0xE4 | rate));
        long per = 512L << rate;
        ok(rate == 0 ? "noise rate 0 shifts at the clock over 512"
         : rate == 1 ? "noise rate 1 shifts at the clock over 1024"
                     : "noise rate 2 shifts at the clock over 2048",
           (double)transitions(3, per * 1000) / 2.0, 1000, 2);
    }
    psg_reset(&p);
    psg_write(&p, 0xC0 | 0x0A); psg_write(&p, 0x11);   /* channel 2 period 0x11A */
    psg_write(&p, 0xE7);                                /* white, rate 3 */
    ok("noise rate 3 shifts at channel 2's period",
       (double)transitions(3, 16L * 0x11A * 2 * 1000) / 2.0, 1000, 2);

    printf("\n%s\n", fails ? "PSG tests FAILED" : "all PSG tests pass");
    return fails ? 1 : 0;
}
"""


def main():
    os.makedirs(OUT, exist_ok=True)
    src = os.path.join(OUT, "psg_test.c")
    exe = os.path.join(OUT, "psg_test" + EXE)
    with open(src, "w") as f:
        f.write(HARNESS.replace("@CLOCK@", str(CLOCK)))
    cmd = [host_cc(), "-O2", "-Wall", "-I", os.path.join(ROOT, "src"),
           "-o", exe, src, os.path.join(ROOT, "src", "psg.c"), "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(" ".join(cmd))
        print(r.stdout + r.stderr)
        return 1
    print("the PSG against the chip's own arithmetic:\n", flush=True)
    return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
