#!/usr/bin/env python3
"""Replay a recorded play session and fail on the things that are always bugs.

  python3 tools/test_session.py
  python3 tools/test_session.py --movie build/other.movie --frames 5000

This is the gate that needs no reference. Every other gate in `make check`
compares against the reference logs, which are 1.7 seconds of a game that runs
for minutes, so nothing has ever gated the part of the run where the bugs have
actually been: the twelve missing blocks found before this, and the thirteenth,
were each one play session to find.

`roms/session.movie` is such a session, recorded with `--record`. It counts our
own frames rather than another emulator's, so it needs no offset and replays
identically for ever — which is what makes a play session into a test.

**It is local, not in the repository**, and lives in `roms/` with the
cartridges and the reference logs for the same reason: it is a large input
somebody has to make for themselves. Make one by playing with `--record` and
saving it there:

    ./build/mars --record roms/session.movie

With no session present this **skips, loudly**. That is the honest failure mode
for an input that may not exist, but it is also exactly the shape of a gate
that passes while measuring nothing — the trap `build/sh2_recomp.c` fell into
when its rule had no prerequisites — so the skip says so in as many words
rather than printing a tick. A `--movie` named explicitly is a different case
and a missing one is fatal.

The session this baseline was taken from is 27,741 frames covering a level, a
**special stage played to its end**, and the Combi Catcher — the only run in
this project that has reached either of the last two. Its numbers are below, so
a differently-recorded session will print deltas against a run it is not; the
loose conditions still hold, and `--rebaseline` prints a replacement block.

**Loose, and the strict numbers printed.** What fails a build is the set of
conditions that are always a bug however the runtime changes: a transfer with
no recompiled block, a CPU parked at zero, an unmapped SH-2 access, the movie
not reaching its end, or the special stage no longer being entered at all.
Everything else — commands by kind, frame-buffer bytes, instruction rates — is
printed against its baseline with the delta, because a scheduling or timing fix
legitimately moves those and a gate that fails on them would cost more than it
catches. A number drifting far is a prompt to look, not a build break.

The one coverage assertion is worth its own line: **the special stage must
still be entered.** The master's interrupt enable going 0x02 -> 0x08 and the
32X vertical interrupt being delivered is something nothing else in this game
does, so it is a cheap proof that the replay still got as deep as it did when
it was recorded, rather than dying quietly in a level and passing the rest.
"""

import argparse
import os
import re
import subprocess
import sys

MOVIE = "roms/session.movie"

# What the session did when it was recorded. Printed and compared, never
# enforced — see the module docstring.
BASELINE = {
    "frames": 27741,
    "cmds_posted": 19644,
    "cmds_serviced": 19427,
    "cmd_kinds": {1: 13, 2: 19287, 3: 12, 4: 3, 7: 1, 8: 1, 9: 327},
    "vints_32x": 6624,
    "fb_bytes": 72383,
    "line_table": 224,
    "vram": 52631,
    "cram": 60,
    "unmapped_68k": 2,
    "unmapped_sh2": 0,
    "master_per_frame": 234994,
    "slave_per_frame": 380556,
    "m68k_per_frame": 11976,
}

# A tolerance only for the printout's "!" marker, not for pass/fail.
LOOSE = 0.10


def run(movie, frames, exe="./build/mars"):
    cmd = [exe, "--movie", movie, "--frames", frames, "--progress", "2000"]
    print("  " + " ".join(cmd))
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.stdout, p.stderr, p.returncode


def parse(out, err):
    r = {}
    def find(pat, cast=int, default=None, text=None):
        m = re.search(pat, text if text is not None else out)
        return cast(m.group(1)) if m else default

    r["played"] = find(r"movie: (\d+) of \d+ frames played")
    r["total"] = find(r"movie: \d+ of (\d+) frames played")
    r["to_end"] = "(to the end)" in out
    r["cmds_posted"] = find(r"commands posted (\d+)")
    r["cmds_serviced"] = find(r"serviced (\d+)")
    kinds = re.search(r"serviced \d+\s+\(([^)]*)\)", out)
    r["cmd_kinds"] = ({int(k): int(v) for k, v in
                       (p.split(":") for p in kinds.group(1).split())}
                      if kinds else {})
    r["fb_bytes"] = find(r"framebuffer: (\d+)/")
    r["line_table"] = find(r"line table: (\d+)/")
    r["vram"] = find(r"genesis: vram (\d+)/")
    r["cram"] = find(r"cram (\d+)/64")
    r["unmapped_68k"] = find(r"unmapped: 68k r=(\d+)")
    r["unmapped_sh2"] = find(r"unmapped: .*sh2=(\d+)")
    r["master_per_frame"] = find(r"master \d+ \((\d+)/frame")
    r["slave_per_frame"] = find(r"slave \d+ \((\d+)/frame")
    r["m68k_per_frame"] = find(r"68000 instructions: \d+ \((\d+)/frame")
    r["master_pc"] = find(r"master 0x([0-9A-F]+)", lambda s: int(s, 16))
    r["slave_pc"] = find(r"slave 0x([0-9A-F]+)", lambda s: int(s, 16))

    # The deepest thing the session does. `--progress` prints "v N/M" per line;
    # the largest N is how many 32X vertical interrupts were delivered, and only
    # the special stage's own setup enables them.
    r["vints_32x"] = max([int(m) for m in re.findall(r" v (\d+)/", out)] or [0])
    r["ie_switched"] = bool(re.search(r"ie 08/", out))

    # Always a bug, wherever it appears.
    r["missing_calls"] = re.findall(r"no recompiled block at 0x([0-9A-F]+)", err)
    r["missing"] = find(r"missing blocks (\d+)", default=0, text=err)
    r["stalls"] = len(re.findall(r"no progress at frame (\d+)", err))
    r["stall_frames"] = re.findall(r"no progress at frame (\d+)", err)
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--movie", default=MOVIE)
    ap.add_argument("--frames", default="movie")
    ap.add_argument("--exe", default="./build/mars")
    ap.add_argument("--rebaseline", action="store_true",
                    help="print a BASELINE block for this session and exit 0")
    args = ap.parse_args()

    # Skipping is a pass, and it says out loud that it measured nothing.
    if args.movie == MOVIE and not os.path.exists(MOVIE):
        print("  session gate SKIPPED — no %s, so this gate measured nothing.\n"
              "  It replays a recorded play session, which is the only gate here\n"
              "  that reaches a level, a special stage or the Combi Catcher. Make\n"
              "  one by playing and saving it where this looks:\n"
              "      ./build/mars --record %s" % (MOVIE, MOVIE))
        return

    out, err, code = run(args.movie, args.frames, args.exe)
    r = parse(out, err)

    if args.rebaseline:
        print("BASELINE = {")
        print('    "frames": %s,' % r["played"])
        for k in ("cmds_posted", "cmds_serviced"):
            print('    "%s": %s,' % (k, r[k]))
        print('    "cmd_kinds": %s,' % dict(sorted(r["cmd_kinds"].items())))
        for k in ("vints_32x", "fb_bytes", "line_table", "vram", "cram",
                  "unmapped_68k", "unmapped_sh2", "master_per_frame",
                  "slave_per_frame", "m68k_per_frame"):
            print('    "%s": %s,' % (k, r[k]))
        print("}")
        return

    fails = []
    if code != 0:
        fails.append("the run exited %d" % code)
    if r["missing_calls"]:
        fails.append("no recompiled block at " +
                     ", ".join("0x" + a for a in r["missing_calls"]))
    if r["played"] is None:
        fails.append("no movie was played — is %s there?" % args.movie)
    elif args.frames == "movie" and not r["to_end"]:
        fails.append("the movie stopped at %s of %s frames"
                     % (r["played"], r["total"]))
    if r["unmapped_sh2"]:
        fails.append("%d unmapped SH-2 access(es)" % r["unmapped_sh2"])
    if not r["master_pc"] or not r["slave_pc"]:
        fails.append("an SH-2 is parked at zero (master 0x%X, slave 0x%X)"
                     % (r["master_pc"] or 0, r["slave_pc"] or 0))
    if not r["cmds_posted"]:
        fails.append("no commands posted at all")
    # Coverage, not accuracy: the session's whole value is that it goes this
    # deep, and a replay that quietly stops short would otherwise pass.
    if args.frames == "movie" and args.movie == MOVIE and not r["vints_32x"]:
        fails.append("the special stage was never entered — no 32X vertical "
                     "interrupt was delivered (interrupt enable never 08)")

    print("\n  %-22s %12s %12s %9s" % ("", "now", "baseline", "delta"))
    for k in ("frames", "cmds_posted", "cmds_serviced", "vints_32x",
              "fb_bytes", "line_table", "vram", "cram", "unmapped_68k",
              "unmapped_sh2", "master_per_frame", "slave_per_frame",
              "m68k_per_frame"):
        now = r["played"] if k == "frames" else r.get(k)
        base = BASELINE[k]
        if now is None:
            print("  %-22s %12s %12d" % (k, "—", base))
            continue
        d = now - base
        mark = " !" if base and abs(d) > LOOSE * base else ""
        print("  %-22s %12d %12d %+9d%s" % (k, now, base, d, mark))
    print("  %-22s %12s %12s" % ("cmd_kinds",
                                 dict(sorted(r["cmd_kinds"].items())),
                                 ""))
    print("  %-22s %12s %12s" % ("", dict(sorted(BASELINE["cmd_kinds"].items())),
                                 "(baseline)"))
    if r["stalls"]:
        # Recovered holds are normal — a results screen looks exactly like one.
        # Reported, never fatal; a terminal one shows up as a parked CPU or as
        # no commands, both of which do fail above.
        print("\n  %d stall report(s), at frame(s) %s — not fatal on their own; "
              "a terminal stall fails above as a parked CPU or no commands"
              % (r["stalls"], ", ".join(r["stall_frames"])))

    if fails:
        print("\nsession gate FAILED:")
        for f in fails:
            print("  * " + f)
        sys.exit(1)
    print("\n  session gate passes — special stage entered (%d 32X vertical "
          "interrupt(s)), no missing block, no unmapped SH-2 access"
          % r["vints_32x"])


if __name__ == "__main__":
    main()
