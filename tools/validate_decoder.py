"""Exhaustively validate the SH-2 decoder against GNU sh-elf-objdump.

Builds a blob containing all 65536 possible instruction words, disassembles it
with objdump, and compares mnemonic + operands word for word. Any disagreement
is a bug in our decode table, which would silently corrupt every downstream
pass, so this runs as a gate before the decoder is trusted.
"""

import re
import subprocess
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sh2.decode import decode, INVALID  # noqa: E402

OBJDUMP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "toolchain", "sh-elf", "bin", "sh-elf-objdump")
TMP = os.environ.get("SCRATCH", "/tmp") + "/sh2_all.bin"


def normalise(s: str) -> str:
    s = s.split("!")[0]                      # drop objdump's trailing comment
    s = re.sub(r"\s+", " ", s).strip()
    s = s.replace(", ", ",")
    return s


def objdump_all(path):
    out = subprocess.run(
        [OBJDUMP, "-D", "-b", "binary", "-m", "sh2", "-EB", "--adjust-vma=0", path],
        capture_output=True, text=True, check=True).stdout
    res = {}
    # "   1234:\t e0 01 \tmov\t#1,r0"
    pat = re.compile(r"^\s*([0-9a-f]+):\s*((?:[0-9a-f]{2} )+)\s*\t(.*)$")
    for line in out.splitlines():
        m = pat.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        raw = m.group(2).split()
        if len(raw) != 2:
            continue
        word = int(raw[0], 16) << 8 | int(raw[1], 16)
        res[addr] = (word, normalise(m.group(3)))
    return res


def main():
    os.makedirs(os.path.dirname(TMP), exist_ok=True)
    with open(TMP, "wb") as f:
        for w in range(0x10000):
            f.write(w.to_bytes(2, "big"))

    ref = objdump_all(TMP)
    print(f"objdump decoded {len(ref)} words")

    mism, invalid_both, checked = [], 0, 0
    for addr, (word, want) in sorted(ref.items()):
        ins = decode(word, addr)
        got = normalise(ins.text())

        want_bad = want.startswith(".word") or want.startswith("bad") or "?" in want
        got_bad = ins.kind == INVALID
        if want_bad and got_bad:
            invalid_both += 1
            continue
        checked += 1

        if got == want:
            continue

        # objdump renders PC-relative loads as the resolved literal address;
        # we keep the architectural @(disp,pc) form. Accept when the pool
        # address we computed matches the address objdump printed.
        if ins.pool is not None:
            m = re.match(r"(mov\.[wl])\s+0x?([0-9a-f]+),(r\d+)", want)
            if m and int(m.group(2), 16) == ins.pool[0] and want.startswith(ins.mnem):
                continue
        if ins.mnem == "mova":
            m = re.match(r"mova\s+0x?([0-9a-f]+),r0", want)
            if m and int(m.group(1), 16) == ins.imm:
                continue
        mism.append((addr, word, want, got))

    print(f"agree on {checked - len(mism)}/{checked} valid words "
          f"({invalid_both} invalid in both)")
    if mism:
        print(f"\n{len(mism)} MISMATCHES (first 40):")
        for addr, word, want, got in mism[:40]:
            print(f"  {word:04x}  objdump={want!r:32} ours={got!r}")
        return 1
    print("decoder matches objdump exactly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
