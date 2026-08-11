#!/usr/bin/env python3
"""Find where our recompiled SH-2 diverges from the reference trace.

  python3 tools/diffsh2.py                 diff the master against the reference
  python3 tools/diffsh2.py --cpu slave     the slave instead
  python3 tools/diffsh2.py --extract       rebuild both reference extracts

Our side comes from `build/mars --trace-sh2 FILE`, which logs one line per basic
block entered, with the whole register state. The reference logs one line per
*instruction*; filtered to the addresses the recompiler knows about, its stream
is the same sequence, so the two compare directly — and between two block
entries both machines ran the same straight-line code, which is what makes a
register difference at a block entry a real one rather than a sampling artefact.

That is also why block granularity is worth having even though it is coarser
than the 68000's: it is not just "did we enter this function", it is the full
state on entry. What it cannot see is *where inside* a block a value first went
wrong, which is the point at which an instrumented codegen mode would earn its
keep.

Everything the reference runs that we have no block for is reported separately:
that is the other half of the question — code the real machine executes and we
never discovered, or never reach.
"""

import argparse
import os
import re
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import tracediff                                       # noqa: E402
import diff68k                                         # noqa: E402
from disasm import analyse                             # noqa: E402
from sh2.decode import decode                          # noqa: E402

DEFAULT_ROM = "roms/Knuckles' Chaotix (JU) (32X) [!].32x"
RECOMP = "build/sh2_recomp.c"
OUR_TRACE = "build/tracesh2.txt"
REF_CACHE = {"master": "build/refshm.txt", "slave": "build/refshs.txt"}
TAG = {"master": "SHM", "slave": "SHS"}
REF_LINES = 400000

# Where the cartridge code the recompiler translated lives. The reference's
# SH-2s start in the adapter's boot ROM below 0x00000300, which we have no image
# of and never execute: our runtime starts them at the cartridge entry points
# instead. So the comparison starts where the reference first leaves the BIOS.
CART_BASE = 0x06000000

NAMES = (["r%d" % i for i in range(16)]
         + ["sr", "gbr", "vbr", "mach", "macl", "pr"])
FIELD = re.compile(r"^(r\d+|sr|gbr|vbr|mach|macl|pr):([0-9a-f]{8})$")
OMITTED = re.compile(r"\[Omitted:\s*(\d+)\]")
TABLE = re.compile(r"\{\s*0x([0-9A-Fa-f]{8})u,\s*f_([0-9A-Fa-f]{8})\s*\}")


def canon(a):
    """The runtime's own address folding: the cached and cache-through views of
    SDRAM and the cartridge are mirrors, the cache arrays above 0x40000000 are
    distinct storage. `sh2_call` does exactly this before looking a block up."""
    return a & 0x1FFFFFFF if a < 0x40000000 else a


# ----------------------------------------------------------------- parsing --
def parse(path, tag, want_text=False):
    """Read one CPU's lines out of a trace. Ours and the reference's differ
    only in that theirs carry a mnemonic between the address and the registers,
    so both are read by scanning for the `name:value` fields rather than by
    counting tokens past a variable-width disassembly."""
    recs, markers, pending = [], {}, []
    gap = 0
    with open(path, "r", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if not line.startswith(tag):
                continue
            body = line.split(" Instruction: ", 1)[-1].strip()
            tok = body.split()
            if not tok:
                continue
            if tok[0] != tag:
                m = OMITTED.search(body)
                if m:
                    gap += int(m.group(1))
                elif "Interrupt" in body:
                    pending.append(body)
                continue
            vals, first = {}, len(tok)
            for k, t in enumerate(tok[2:], 2):
                m = FIELD.match(t)
                if m:
                    first = min(first, k)
                    vals[m.group(1)] = int(m.group(2), 16)
            if len(vals) != len(NAMES):
                continue
            try:
                pc = canon(int(tok[1], 16))
            except ValueError:
                continue
            text = " ".join(tok[2:first]) if want_text else ""
            recs.append(tracediff.Rec(pc, None, tuple(vals[k] for k in NAMES),
                                      tok[-1], text, n, gap))
            if pending:
                markers[n] = pending
                pending = []
            gap = 0
    return recs, markers


def load_model(recomp, rom_path):
    """What the recompiler translated: which addresses start a block, and which
    addresses it covers at all.

    The block set is read out of the generated C, because that is what the
    binary actually dispatches to. The covered-instruction set comes from the
    analysis the generator ran on, since the table does not record how far a
    block extends. They are derived from the same pass, so a disagreement means
    the checked-in build is stale — worth saying rather than silently trusting
    one of them.
    """
    if not os.path.exists(recomp):
        sys.exit("%s not found — run `python3 tools/recompile.py` first"
                 % recomp)
    with open(recomp) as f:
        blocks = {int(b, 16): int(fn, 16)
                  for b, fn in TABLE.findall(f.read())}

    _, _, _, az = analyse(rom_path)
    covered, length = set(), {}
    for a in az.funcs:
        for b in az.funcs[a].blocks.values():
            # A basic block always executes whole, so its instruction count is
            # exactly what our side ran on entering it. That is what makes a
            # trip count comparable against the reference's instruction counts.
            length[b.start] = len(b.insns)
            covered.update(i.addr for i in b.insns)
    if set(length) != set(blocks):
        print("  ! %s has %d blocks, the analysis finds %d — rebuild it"
              % (recomp, len(blocks), len(length)))
    return blocks, covered, length


def filter_blocks(recs, blocks, covered):
    """Keep the reference lines that are block entries; everything else is an
    instruction inside a block, and folds into the gap count.

    An address that is in no block at all is the interesting leftover: code the
    real machine runs that the recompiler has never seen. Those are counted
    separately and reported rather than quietly skipped.
    """
    out, unknown, gap = [], Counter(), 0
    for r in recs:
        gap += r.gap
        if r.pc in blocks:
            r.gap = gap
            gap = 0
            out.append(r)
            continue
        if r.pc not in covered:
            unknown[r.pc] += 1
        gap += 1
    return out, unknown


# -------------------------------------------------------------- extraction --
def cache_paths(prefix):
    """Where an extract lands. `make check` gates on the default pair, so a
    longer extract — which is a different artefact, not a newer one — is asked
    for by name and leaves those alone."""
    return {"master": prefix + "shm.txt", "slave": prefix + "shs.txt"}


def extract(rom_path, limit, cache=None):
    """Copy both SH-2 streams forward from the console reset.

    The reset itself is found through the 68000, whose reset vector is the one
    address in the logs we know from the cartridge header. The SH-2s reach
    cartridge code some way after it — the master spends 9.4 million
    instructions summing the cartridge in BIOS first, nearly all of it collapsed
    into a single omitted run — so each stream starts at its first instruction
    at or above 0x06000000, which is the master's 0x060001A0 and the slave's
    0x060001A4: exactly the two entry points our runtime uses.
    """
    cache = cache or REF_CACHE
    pc = diff68k.find_reset_pc(rom_path)
    first, at, logs = diff68k.find_reset(pc)

    outs = {t: open(cache[k], "w") for k, t in TAG.items()}
    started = {t: False for t in outs}
    n = {t: 0 for t in outs}
    scanned = 0
    try:
        for k in range(first, len(logs)):
            with open(logs[k], "rb") as f:
                if k == first:
                    f.seek(at)
                for raw in f:
                    scanned += 1
                    tag = raw[:3].decode("latin1")
                    out = outs.get(tag)
                    if out is None or n[tag] >= limit:
                        continue
                    line = raw.decode("latin1")
                    tok = line.split(" Instruction: ", 1)[-1].split()
                    if tok and tok[0] == tag:
                        try:
                            pc = canon(int(tok[1], 16))
                        except (IndexError, ValueError):
                            continue
                        # The gate is only about *where to start*. Once the CPU
                        # has left the BIOS, everything it runs is kept — it
                        # goes on to execute the cache-array overlay at
                        # 0xC0000000 and cartridge space at 0x0208xxxx, and
                        # dropping either would hide a divergence rather than
                        # report one.
                        if not started[tag]:
                            if pc < CART_BASE:
                                continue
                            started[tag] = True
                            print("  %s enters cartridge code at 0x%08X, "
                                  "%d log lines past the reset"
                                  % (tag, pc, scanned))
                    elif not started[tag]:
                        continue          # an omitted run of BIOS code
                    out.write(line)
                    n[tag] += 1
            print("  through %s: %s" % (os.path.basename(logs[k]),
                                        ", ".join("%s %d" % (t, n[t])
                                                  for t in sorted(n))))
            if all(v >= limit for v in n.values()):
                break
    finally:
        for out in outs.values():
            out.close()
    for k, t in sorted(TAG.items()):
        print("  wrote %s (%d lines)" % (cache[k], n[t]))


# ------------------------------------------------------------- annotation ---
class Disasm:
    """Annotate an SH-2 address from the images the runtime itself builds: the
    SDRAM copy the 32X boot ROM stages out of the cartridge, the cache-array
    overlay, and the cartridge as seen at 0x02000000."""

    def __init__(self, rom_path):
        with open(rom_path, "rb") as f:
            self.rom = f.read()
        be = lambda o: int.from_bytes(self.rom[o:o + 4], "big")
        self.src, self.dst, self.size = be(0x3D4), be(0x3D8), be(0x3DC)

    def offset(self, a):
        sd = CART_BASE + self.dst
        if sd <= a < sd + self.size:
            return self.src + (a - sd)
        if 0xC0000000 <= a < 0xC0000400:
            return 0x07FC00 + (a - 0xC0000000)
        if 0x02000000 <= a < 0x02000000 + len(self.rom):
            return a - 0x02000000
        return None

    def at(self, a):
        off = self.offset(a)
        if off is None or off + 1 >= len(self.rom):
            return "(not in any image)"
        try:
            return decode(int.from_bytes(self.rom[off:off + 2], "big"), a).text()
        except Exception:
            return "(undecodable)"


def show_regs(rec):
    r = rec.regs
    return ["       r0-7   " + " ".join("%08x" % v for v in r[:8]),
            "       r8-15  " + " ".join("%08x" % v for v in r[8:16]),
            "       " + " ".join("%s:%08x" % (n, v)
                                 for n, v in zip(NAMES[16:], r[16:]))
            + "  " + rec.flags]


def report_unknown(unknown, dis, rows):
    """Reference addresses the recompiler covers nowhere — the direct answer to
    "does the reference run code we never discovered"."""
    if not unknown:
        print("\nevery instruction the reference ran is inside a block we have")
        return
    total = sum(unknown.values())
    print("\n%d reference instruction(s) at %d address(es) in no block of "
          "ours:" % (total, len(unknown)))
    for a, c in unknown.most_common(rows):
        print("    %08x  x%-8d %s" % (a, c, dis.at(a)))
    if len(unknown) > rows:
        print("    ... %d more addresses" % (len(unknown) - rows))


# ------------------------------------------------------------------- main ---
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=DEFAULT_ROM)
    ap.add_argument("--cpu", choices=("master", "slave"), default="master")
    ap.add_argument("--ref", default=None)
    ap.add_argument("--ours", default=OUR_TRACE)
    ap.add_argument("--recomp", default=RECOMP)
    ap.add_argument("--extract", action="store_true",
                    help="rebuild both reference extracts from roms/*.log")
    ap.add_argument("--limit", type=int, default=REF_LINES,
                    help="reference instructions to extract, per CPU")
    ap.add_argument("--extract-to", default="build/ref", metavar="PREFIX",
                    help="where an extract is written; PREFIX+'shm.txt' and "
                         "PREFIX+'shs.txt'. The default pair is what the gates "
                         "read, so a longer extract wants its own prefix.")
    ap.add_argument("--ref-blocks", type=int, default=0,
                    help="compare only the first N reference block entries")
    ap.add_argument("--context", type=int, default=12,
                    help="reference blocks of lead-up to print")
    ap.add_argument("--window", type=int, default=40000,
                    help="how far forward in our stream to look for a match")
    ap.add_argument("--hold", type=int, default=4,
                    help="blocks that must keep matching to accept a spot")
    ap.add_argument("--skip", type=int, default=400,
                    help="reference blocks we may never enter and still call it"
                         " a rejoin")
    ap.add_argument("--rows", type=int, default=40,
                    help="summary rows to print before truncating")
    ap.add_argument("--detail", type=int, default=2,
                    help="non-fatal divergences to print in full")
    args = ap.parse_args()

    cache = cache_paths(args.extract_to)
    ref_path = args.ref or cache[args.cpu]
    if args.extract or not all(os.path.exists(p) for p in cache.values()):
        print("extracting reference traces:")
        extract(args.rom, args.limit, cache)

    tag = TAG[args.cpu]
    dis = Disasm(args.rom)
    cpu = tracediff.Cpu(NAMES, 8, dis.at, show_regs)
    blocks, covered, length = load_model(args.recomp, args.rom)

    raw, marks = parse(ref_path, tag, want_text=True)
    ref, unknown = filter_blocks(raw, blocks, covered)
    ours, _ = parse(args.ours, tag)
    for r in ours:
        r.ran = length.get(r.pc, 1)
    if args.ref_blocks:
        ref = ref[:args.ref_blocks]
    print("%s: %d block(s) known to the recompiler" % (args.cpu, len(blocks)))
    print("reference %d instructions -> %d block entries; ours %d"
          % (len(raw), len(ref), len(ours)))
    if not ref or not ours:
        report_unknown(unknown, dis, args.rows)
        sys.exit("nothing to compare — is %s non-empty for %s?"
                 % (args.ours, tag))

    divs, agreed = tracediff.diff(ref, ours, args.window, args.hold, args.skip)
    rc = tracediff.report(cpu, ref, ours, marks, divs, agreed, args,
                          unit="block", ran_unit="instruction")
    report_unknown(unknown, dis, args.rows)
    return rc


if __name__ == "__main__":
    sys.exit(main())
