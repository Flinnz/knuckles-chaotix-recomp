"""Exhaustively validate the 68000 decoder against GNU m68k-elf-objdump.

68000 instructions are variable length, so a flat blob of candidate opcodes
cannot be compared position by position — one wrong length and everything after
it is off. The fix is to give each of the 65536 opcode words its own 12-byte
slot padded with 0x4E71 (`nop`). Whatever the instruction under test consumes,
the remaining words each decode as a single-word nop, so objdump lands exactly
on the next slot boundary and every candidate is compared independently.

Length is checked as strictly as the text: a wrong length desynchronises real
disassembly even when the mnemonic is right.
"""

import os
import re
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m68k.decode import decode, INVALID  # noqa: E402

OBJDUMP = "m68k-elf-objdump"
SLOT = 12
NOP = 0x4E71
TMP = os.environ.get("SCRATCH", "/tmp") + "/m68k_all.bin"


def normalise(s):
    s = re.sub(r"\s+", " ", s).strip()
    return s.replace(", ", ",")


def objdump_slots(path):
    """Map slot address -> (text, byte length), folding continuation lines."""
    out = subprocess.run(
        [OBJDUMP, "-D", "-b", "binary", "-m", "m68k:68000", "-EB",
         "--adjust-vma=0", path],
        capture_output=True, text=True, check=True).stdout
    pat = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f ]+?)\s*(?:\t(.*))?$")
    res, cur = {}, None
    for line in out.splitlines():
        m = pat.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        nbytes = sum(len(tok) for tok in m.group(2).split()) // 2
        text = (m.group(3) or "").strip()
        if not text:
            # A wrapped byte line: those bytes belong to the previous entry.
            if cur is not None:
                t, n = res[cur]
                res[cur] = (t, n + nbytes)
            continue
        res[addr] = (normalise(text), nbytes)
        cur = addr
    return res


def main():
    os.makedirs(os.path.dirname(TMP), exist_ok=True)
    pad = NOP.to_bytes(2, "big") * ((SLOT - 2) // 2)
    with open(TMP, "wb") as f:
        for wv in range(0x10000):
            f.write(wv.to_bytes(2, "big") + pad)

    ref = objdump_slots(TMP)
    print(f"objdump produced {len(ref):,} entries")
    data = open(TMP, "rb").read()

    def fetch(a):
        return int.from_bytes(data[a:a + 2], "big")

    len_bad, text_bad, both_invalid, checked = [], [], 0, 0
    coproc = accepted = 0
    for wv in range(0x10000):
        base = wv * SLOT
        if base not in ref:
            continue
        if (wv >> 12) in (0xA, 0xF):
            coproc += 1          # line A / line F: traps on a bare 68000
            continue
        want_text, want_len = ref[base]
        ins = decode(fetch, base)
        got_text = normalise(ins.text())
        want_invalid = (want_text.startswith(".short") or want_text.startswith(".word")
                        or "bad" in want_text)
        if want_invalid and ins.kind == INVALID:
            both_invalid += 1
            continue
        # Accepted divergences from objdump, both deliberate:
        #  * `swbeg` is a GNU assembler pseudo-op, not a 68000 instruction.
        #  * ADDQ/SUBQ to an address register is word/long only on real
        #    hardware; objdump decodes the byte form, we reject it so that
        #    discovery cannot manufacture code out of data.
        if want_text.startswith("swbeg") or (
                want_text.startswith("subqb") and ",%a" in want_text
                or want_text.startswith("subqb") and (",%fp" in want_text
                                                      or ",%sp" in want_text)):
            accepted += 1
            continue
        checked += 1
        if ins.size != want_len:
            len_bad.append((wv, want_text, want_len, got_text, ins.size))
        elif got_text != want_text:
            text_bad.append((wv, want_text, got_text))

    good = checked - len(len_bad) - len(text_bad)
    print(f"checked {checked:,} words  ({both_invalid:,} invalid in both, "
          f"{coproc:,} line-A/F coprocessor space skipped, "
          f"{accepted:,} accepted divergences)")
    print(f"  length mismatches : {len(len_bad):,}")
    print(f"  text   mismatches : {len(text_bad):,}")
    print(f"  exact agreement   : {good:,} ({100.0 * good / max(checked, 1):.2f}%)")

    if len_bad:
        print("\nLENGTH mismatch classes:")
        c = Counter((t.split()[0], (g.split() or [""])[0], wl, gl)
                    for _, t, wl, g, gl in len_bad)
        for (wm, gm, wl, gl), n in c.most_common(15):
            print(f"  objdump {wm:<12} {wl}B | ours {gm:<12} {gl}B   x{n}")

    if text_bad:
        print("\nTEXT mismatch classes:")
        c = Counter((t.split()[0], (g.split() or [""])[0]) for _, t, g in text_bad)
        for (wm, gm), n in c.most_common(20):
            print(f"  objdump {wm:<12} ours {gm:<12} x{n}")
        for wv, t, g in text_bad[:10]:
            print(f"    {wv:04x}: objdump {t!r}")
            print(f"           ours    {g!r}")
    return 0 if not (len_bad or text_bad) else 1


if __name__ == "__main__":
    sys.exit(main())
