#!/usr/bin/env python3
"""End-to-end test of the SH-2 recompiler.

Assembles an SH-2 test program, runs it through discovery and code generation,
compiles the result for this machine, executes it, and checks the answers.

The expected values are all computed independently of any SH-2 model — a
division with a known quotient, a loop with a countable trip count — so a
failure indicts the recompiler, not the oracle.
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mars                                    # noqa: E402
from sh2.analyze import Analyzer, Image        # noqa: E402
from recomp.sh2c import Codegen, fname         # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "toolchain", "sh-elf", "bin")
BASE = 0x06000000
OUT = os.path.join(ROOT, "build", "test")

CASES = [
    ("1000 / 7 quotient  (div1 x32)", 142),
    ("1000 / 7 remainder (div1 x32)", 6),
    ("delay slot runs on untaken branch", 5),
    ("delay slot must not clobber jsr target", 7),
    ("64-bit add, low word (addc carry out)", 1),
    ("64-bit add, high word (addc carry in)", 5),
    ("shar of -8 (arithmetic)", 0xFFFFFFFC),
    ("shlr of -8 (logical)", 0x7FFFFFFC),
]


def sh(*cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(" ".join(cmd))
        print(r.stdout + r.stderr)
        sys.exit(1)
    return r.stdout


def main():
    os.makedirs(OUT, exist_ok=True)
    src = os.path.join(ROOT, "tests", "sh2", "arith.s")
    obj, elf, binf = (os.path.join(OUT, n) for n in ("arith.o", "arith.elf", "arith.bin"))

    # Link at the SDRAM base so literal-pool pointers hold runtime addresses.
    sh(os.path.join(BIN, "sh-elf-as"), "-big", "-isa=sh2", "-o", obj, src)
    sh(os.path.join(BIN, "sh-elf-ld"), "-Ttext", hex(BASE), "-o", elf, obj)
    # Without -j, objcopy takes its origin from address 0 and pads out the
    # whole gap to the link address.
    sh(os.path.join(BIN, "sh-elf-objcopy"), "-O", "binary", "-j", ".text",
       elf, binf)
    blob = open(binf, "rb").read()
    print(f"assembled {len(blob)} bytes at 0x{BASE:08X}")

    img = Image(norm=mars.sh2_phys)
    img.add(BASE, blob)
    az = Analyzer(img, is_code_addr=lambda a: not (a & 1) and
                  BASE <= mars.sh2_phys(a) < BASE + len(blob))
    az.add_function(BASE, "test entry")
    az.run()
    print(f"discovered {len(az.funcs)} function(s), {len(az.code)} instructions")

    cg = Codegen(az, img)
    entries = sorted(az.funcs)
    lines = ['#include "sh2.h"', ""]
    lines += [f"void {fname(a)}(SH2 *c);" for a in entries] + [""]
    for a in entries:
        lines += cg.function(az.funcs[a]) + [""]
    lines += ["typedef struct { uint32_t addr; void (*fn)(SH2 *); } SH2Entry;",
              "const SH2Entry sh2_functions[] = {"]
    lines += [f"    {{ 0x{a:08X}u, {fname(a)} }}," for a in entries]
    lines += ["};", f"const unsigned sh2_function_count = {len(entries)};"]
    cfile = os.path.join(OUT, "arith_recomp.c")
    with open(cfile, "w") as f:
        f.write("\n".join(lines) + "\n")
    if cg.notes:
        print("  unhandled:", cg.notes)

    exe = os.path.join(OUT, "arith")
    sh("clang", "-O2", "-Wall", "-Wno-unused-label", f"-I{ROOT}/src",
       "-o", exe, cfile, os.path.join(ROOT, "src", "sh2_testmem.c"))
    got = [int(x) & 0xFFFFFFFF for x in
           sh(exe, binf, str(len(CASES))).split()]

    print()
    ok = True
    for (label, want), val in zip(CASES, got):
        good = val == want
        ok &= good
        print(f"  {'PASS' if good else 'FAIL'}  {label:<42} "
              f"got {val:<12} want {want}")
    print("\n" + ("all SH-2 semantics tests pass" if ok else "FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
