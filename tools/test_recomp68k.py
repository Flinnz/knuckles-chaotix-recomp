#!/usr/bin/env python3
"""End-to-end test of the 68000 recompiler, against Musashi.

Assembles a 68000 test program, runs it through discovery and code generation,
compiles the result, and executes the same program twice over identical
memory — once interpreted, once recompiled — comparing the value *and the
condition codes* each case leaves behind.

The oracle is deliberately not a table of expected numbers. The values are the
easy part; V and C are where a reading of the manual goes wrong, and an
independent implementation of the same instruction set disagrees exactly where
a misreading would.
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from hostcc import EXE, host_cc                  # noqa: E402
from m68k.analyze import Analyzer                # noqa: E402
from recomp.m68kc import Codegen, fname          # noqa: E402
from recompile68k import CYCLES_PATH, read_cycles # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "test")
MUSASHI = os.path.join(ROOT, "third_party", "musashi")
GEN = os.path.join(ROOT, "build", "musashi")
BASE = 0x1000
FLAGS = "XNZVC"


def sh(*cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(" ".join(cmd))
        print(r.stdout + r.stderr)
        sys.exit(1)
    return r.stdout


def symbols(elf):
    out = sh("m68k-elf-nm", elf)
    return {name: int(addr, 16)
            for addr, _kind, name in (l.split() for l in out.splitlines()
                                      if len(l.split()) == 3)}


def case_labels(src):
    """The comment above each `bsr record` names the case it belongs to."""
    names, pending = [], None
    for line in open(src):
        s = line.strip()
        if s.startswith("|") and not s.startswith("| ---"):
            pending = s.lstrip("| ").strip()
        elif s.startswith("bsr") and "record" in s:
            names.append(pending or f"case {len(names) + 1}")
            pending = None
        elif s and not s.startswith("|"):
            m = re.search(r"\|\s*(.+)$", line)
            if m:
                pending = m.group(1).strip()
    return names


def main():
    os.makedirs(OUT, exist_ok=True)
    src = os.path.join(ROOT, "tests", "m68k", "arith.s")
    obj, elf, binf = (os.path.join(OUT, n) for n in
                      ("arith68.o", "arith68.elf", "arith68.bin"))

    sh("m68k-elf-as", "-m68000", "-o", obj, src)
    sh("m68k-elf-ld", "-Ttext", hex(BASE), "-o", elf, obj)
    sh("m68k-elf-objcopy", "-O", "binary", "-j", ".text", elf, binf)
    blob = open(binf, "rb").read()
    sym = symbols(elf)
    print(f"assembled {len(blob)} bytes at 0x{BASE:04X}, "
          f"results at 0x{sym['results']:04X}")

    # Discovery works in offset space and an address below 0x400000 is its own
    # offset, so padding the blob out to its link address makes the two the
    # same thing here too.
    data = bytes(BASE) + blob
    az = Analyzer(data)
    az.add_function(sym["start"], "test entry")
    az.run()
    print(f"discovered {len(az.funcs)} function(s), {len(az.code)} instructions")

    # These cases compare answers rather than timings, so the cycle table is
    # here only because a block has to carry a cost — but taking the real one
    # keeps this compiling the same C the runtime does.
    cg = Codegen(az, read_cycles(CYCLES_PATH))
    entries = sorted(az.funcs)
    lines = ['#include "m68000.h"', ""]
    lines += [f"static uint32_t {fname(a)}(M68K *c, uint32_t entry);"
              for a in entries] + [""]
    for a in entries:
        lines += cg.function(az.funcs[a]) + [""]
    owner = {}
    for a in entries:
        for b in az.funcs[a].blocks:
            if b not in owner or a < owner[b]:
                owner[b] = a
    rows = sorted(owner.items())
    lines += ["const M68KEntry m68k_functions[] = {"]
    lines += [f"    {{ 0x{b:08X}u, {fname(a)} }}," for b, a in rows]
    lines += ["};", f"const unsigned m68k_function_count = {len(rows)};"]
    cfile = os.path.join(OUT, "arith68_recomp.c")
    with open(cfile, "w") as f:
        f.write("\n".join(lines) + "\n")
    if cg.notes:
        print("  outside the model:", cg.notes)

    exe = os.path.join(OUT, "arith68" + EXE)
    sh(host_cc(), "-O2", "-Wall", "-Wno-unused-label",
       "-include", os.path.join(ROOT, "src", "m68kconf.h"),
       f"-I{ROOT}/src", f"-I{MUSASHI}", f"-I{GEN}", "-o", exe,
       cfile, os.path.join(ROOT, "src", "m68000.c"),
       os.path.join(ROOT, "src", "m68k_testmem.c"),
       os.path.join(MUSASHI, "m68kcpu.c"), os.path.join(GEN, "m68kops.c"),
       os.path.join(MUSASHI, "softfloat", "softfloat.c"))

    names = case_labels(src)
    out = sh(exe, binf, hex(BASE), hex(sym["results"]), str(len(names)))
    got = {}
    for line in out.splitlines():
        tag, rest = line.split(None, 1)
        got[tag] = rest.split()
    if "musashi" not in got or "recomp" not in got:
        print(out)
        return 1

    print()
    ok = True
    for i, name in enumerate(names):
        a, b = got["musashi"][i], got["recomp"][i]
        good = a == b
        ok &= good
        av, af = a.split(":")
        bv, bf = b.split(":")
        note = "" if good else f"   musashi {a}  recomp {b}"
        print(f"  {'PASS' if good else 'FAIL'}  {name[:46]:<46} "
              f"{av} {flags(int(af, 16))}{note}")
    print(f"\n{len(names)} case(s); "
          + ("all 68000 semantics tests pass" if ok else "FAILURES"))
    return 0 if ok else 1


def flags(v):
    return "".join(f if v & (1 << (4 - i)) else "-" for i, f in enumerate(FLAGS))


if __name__ == "__main__":
    sys.exit(main())
