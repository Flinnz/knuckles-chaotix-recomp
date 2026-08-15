#!/usr/bin/env python3
"""Turn a BizHawk .bk2 movie into a per-frame input file the runtime can read.

  python3 tools/bk2.py roms/foo.bk2                  -> build/foo.movie
  python3 tools/bk2.py roms/foo.bk2 -o build/m.txt
  python3 tools/bk2.py roms/foo.bk2 --summary        just say what is in it

Then: ./build/mars --movie build/foo.movie --frames movie

A .bk2 is a zip. `Input Log.txt` inside it is one line per frame, sections
separated by `|`, one character per button — the character itself where the
button is down and `.` where it is up. `LogKey:` on the second line names every
column in order, so the mapping from column to button is read out of the movie
rather than assumed: a movie recorded with a different controller, or with the
two players the other way round, still comes out right.

What this emits is two 12-bit masks per frame, in the bit order src/mars.h
defines for `gen.pad_buttons`, so the runtime needs no name table of its own and
there is one place where a column means a button. Text rather than packed bytes
because every other artefact this project generates is greppable, and 111,397
frames is under a megabyte either way.

Two things a .bk2 can hold that the runtime cannot honour, and both are reported
rather than passed through quietly:

* **Power and Reset.** A movie may reset the console mid-run. Nothing here can,
  so a movie that uses either is a movie whose replay is wrong from that frame
  and it says so.
* **A starting savestate.** A `Core` member means the movie begins from a state
  rather than from power-on, and replaying its input from frame zero is
  meaningless. Ours has no such member — the archive is the seven files a
  power-on movie has.

The frame numbering is the movie's, which counts from power-on and therefore
includes however long the console's own boot ROM takes before the cartridge gets
control. This runtime stands in for that boot ROM instead of running it, so its
frame zero is not the movie's frame zero; `--movie-offset` is what shifts one
onto the other. See the runtime's own note on that.
"""

import argparse
import os
import sys
import zipfile

# The twelve lines of a six-button pad, as src/mars.h numbers them. Keyed by the
# button name BizHawk's LogKey uses, less the player prefix.
BITS = {
    "Up": 0x001, "Down": 0x002, "Left": 0x004, "Right": 0x008,
    "A": 0x010, "B": 0x020, "C": 0x040, "Start": 0x080,
    "X": 0x100, "Y": 0x200, "Z": 0x400, "Mode": 0x800,
}

PORTS = 2               # what the runtime models as pads holding something


def parse_logkey(key):
    """Column index -> (port, bit), and the width of each `|` section.

    LogKey names every column in order, `#` starting a new section. The input
    lines are the same sections in the same order, so the widths are what says
    which characters belong to which player.
    """
    assert key.startswith("LogKey:"), key
    sections = [s for s in key[len("LogKey:"):].split("#") if s]
    widths, cols = [], []
    for sec in sections:
        names = [n for n in sec.split("|") if n]
        widths.append(len(names))
        cols.append(names)
    return widths, cols


def load(path):
    """(frames, control_used, header) from a .bk2.

    `frames` is a list of one tuple of `PORTS` masks per movie frame.
    """
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        if "Core" in names:
            sys.exit("%s begins from a savestate (`Core` member); its input "
                     "cannot be replayed from power-on" % path)
        header = {}
        for line in z.read("Header.txt").decode("utf-8", "replace").splitlines():
            k, _, v = line.partition(" ")
            if k:
                header[k] = v
        log = z.read("Input Log.txt").decode("utf-8", "replace").splitlines()

    if not log or log[0].strip() != "[Input]":
        sys.exit("%s: no [Input] section" % path)
    widths, cols = parse_logkey(log[1])

    # Which (section, column) is which port's which button, and which columns
    # are the console's own controls rather than anyone's pad.
    where, control = {}, {}
    for s, names in enumerate(cols):
        for c, name in enumerate(names):
            player, _, button = name.partition(" ")
            if player in ("Power", "Reset"):
                control[(s, c)] = player
            elif player.startswith("P") and player[1:].isdigit():
                port = int(player[1:]) - 1
                if port < PORTS and button in BITS:
                    where[(s, c)] = (port, BITS[button])

    frames, control_used = [], {}
    for n, line in enumerate(log[2:]):
        if line == "[/Input]" or not line.strip():
            continue
        if not line.startswith("|"):
            sys.exit("%s: frame %d is not an input line: %r" % (path, n, line))
        secs = line.strip("|").split("|")
        if [len(s) for s in secs] != widths:
            sys.exit("%s: frame %d has sections %r, LogKey says %r"
                     % (path, n, [len(s) for s in secs], widths))
        mask = [0] * PORTS
        for s, sec in enumerate(secs):
            for c, ch in enumerate(sec):
                if ch == ".":
                    continue
                if (s, c) in where:
                    port, bit = where[(s, c)]
                    mask[port] |= bit
                elif (s, c) in control:
                    control_used.setdefault(control[(s, c)], []).append(len(frames))
        frames.append(tuple(mask))
    return frames, control_used, header


def summarise(frames, control_used, header, path):
    print("%s: %d frames (%.1f s at 60 Hz)"
          % (os.path.basename(path), len(frames), len(frames) / 60.0))
    for k in ("Core", "Platform", "GameName", "Is32X", "rerecordCount"):
        if k in header:
            print("  %-14s %s" % (k, header[k]))
    for port in range(PORTS):
        held = {name: sum(1 for f in frames if f[port] & bit)
                for name, bit in BITS.items()}
        used = {n: c for n, c in held.items() if c}
        active = sum(1 for f in frames if f[port])
        print("  port %d: %d frame(s) with anything held; %s"
              % (port + 1, active,
                 ", ".join("%s %d" % (n, c) for n, c in sorted(
                     used.items(), key=lambda kv: -kv[1])) or "nothing"))
    for name, at in sorted(control_used.items()):
        print("  ! %s used on %d frame(s), first %d — the runtime cannot honour "
              "it, so the replay is wrong from there" % (name, len(at), at[0]))


def write(frames, out, path, header):
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w") as f:
        f.write("# %s\n" % os.path.basename(path))
        f.write("# frames %d\n" % len(frames))
        for k in ("Core", "Platform", "GameName"):
            if k in header:
                f.write("# %s %s\n" % (k.lower(), header[k]))
        f.write("# one line per frame: port 1 mask, port 2 mask, hex\n")
        f.write("# 001 up 002 down 004 left 008 right 010 a 020 b 040 c "
                "080 start 100 x 200 y 400 z 800 mode\n")
        for m in frames:
            f.write("%03X %03X\n" % m)
    print("  wrote %s (%d frames)" % (out, len(frames)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bk2")
    ap.add_argument("-o", "--out", help="default build/<name>.movie")
    ap.add_argument("--summary", action="store_true",
                    help="report what is in the movie and write nothing")
    args = ap.parse_args()

    frames, control_used, header = load(args.bk2)
    summarise(frames, control_used, header, args.bk2)
    if not args.summary:
        out = args.out or os.path.join(
            "build", os.path.splitext(os.path.basename(args.bk2))[0] + ".movie")
        write(frames, out, args.bk2, header)


if __name__ == "__main__":
    main()
