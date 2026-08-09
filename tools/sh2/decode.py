"""Hitachi SH-2 instruction decoder.

Fixed 16-bit big-endian encoding. Decoding produces an `Insn` carrying both a
printable form and the semantic facts the recompiler needs: control-flow class,
statically-known branch target, delay-slot flag, and PC-relative literal
references (SH-2 materialises every constant and address through a literal pool,
so resolving those is what turns a byte blob into a call graph).

Syntax follows GNU `sh-elf-objdump` so output can be diffed against it.
"""

from dataclasses import dataclass, field
from typing import Optional

# Control-flow classes.
NORMAL = "normal"      # falls through
BRANCH = "branch"      # conditional branch, target known
JUMP = "jump"          # unconditional transfer, target known
JUMP_IND = "jump_ind"  # unconditional transfer through a register
CALL = "call"          # bsr / jsr — pushes PR
CALL_IND = "call_ind"
RET = "ret"            # rts / rte
INVALID = "invalid"


@dataclass
class Insn:
    addr: int
    word: int
    mnem: str = ".word"
    ops: str = ""
    kind: str = NORMAL
    target: Optional[int] = None   # resolved branch/call destination
    delay: bool = False            # instruction has a delay slot
    # Literal-pool reference: (address_of_literal, size_in_bytes). The pointed-to
    # value is the operand actually loaded.
    pool: Optional[tuple] = None
    rn: Optional[int] = None
    rm: Optional[int] = None
    imm: Optional[int] = None
    # Register liveness, as indices 0..15 plus pseudo-names for the control regs.
    reads: tuple = ()
    writes: tuple = ()

    @property
    def size(self) -> int:
        return 2

    def text(self) -> str:
        return f"{self.mnem}\t{self.ops}" if self.ops else self.mnem

    def __str__(self) -> str:
        return f"{self.addr:08x}: {self.word:04x}  {self.text()}"


def _s8(v):
    return v - 0x100 if v & 0x80 else v


def _s12(v):
    return v - 0x1000 if v & 0x800 else v


def _simm8(v):
    """8-bit immediate, sign-extended (mov #imm,Rn / add #imm,Rn)."""
    return _s8(v)


def R(i):
    return f"r{i}"


def decode(word: int, addr: int) -> Insn:
    """Decode one 16-bit instruction word located at `addr`."""
    n = (word >> 8) & 0xF
    m = (word >> 4) & 0xF
    d4 = word & 0xF
    d8 = word & 0xFF
    d12 = word & 0xFFF
    op = (word >> 12) & 0xF

    def I(mnem, ops="", **kw):
        return Insn(addr=addr, word=word, mnem=mnem, ops=ops, **kw)

    # ---------------------------------------------------------------- 0x0
    if op == 0x0:
        lo = word & 0xFF
        low4 = word & 0xF
        if low4 == 0x4:
            return I("mov.b", f"{R(m)},@(r0,{R(n)})", rn=n, rm=m, reads=(m, n, 0))
        if low4 == 0x5:
            return I("mov.w", f"{R(m)},@(r0,{R(n)})", rn=n, rm=m, reads=(m, n, 0))
        if low4 == 0x6:
            return I("mov.l", f"{R(m)},@(r0,{R(n)})", rn=n, rm=m, reads=(m, n, 0))
        if low4 == 0x7:
            return I("mul.l", f"{R(m)},{R(n)}", rn=n, rm=m, reads=(m, n), writes=("macl",))
        if low4 == 0xC:
            return I("mov.b", f"@(r0,{R(m)}),{R(n)}", rn=n, rm=m, reads=(m, 0), writes=(n,))
        if low4 == 0xD:
            return I("mov.w", f"@(r0,{R(m)}),{R(n)}", rn=n, rm=m, reads=(m, 0), writes=(n,))
        if low4 == 0xE:
            return I("mov.l", f"@(r0,{R(m)}),{R(n)}", rn=n, rm=m, reads=(m, 0), writes=(n,))
        if low4 == 0xF:
            return I("mac.l", f"@{R(m)}+,@{R(n)}+", rn=n, rm=m,
                     reads=(m, n), writes=(m, n, "mach", "macl"))
        # Zero-format: the whole 16-bit word is fixed, the n field is not a
        # register here and must be zero.
        if word == 0x0008:
            return I("clrt", writes=("t",))
        if word == 0x0018:
            return I("sett", writes=("t",))
        if word == 0x0028:
            return I("clrmac", writes=("mach", "macl"))
        if word == 0x0009:
            return I("nop")
        if word == 0x0019:
            return I("div0u", writes=("t", "q", "m"))
        if word == 0x000B:
            return I("rts", kind=RET, delay=True, reads=("pr",))
        if word == 0x001B:
            return I("sleep")
        if word == 0x002B:
            return I("rte", kind=RET, delay=True)
        if (word & 0xF0FF) == 0x0029:
            return I("movt", R(n), rn=n, reads=("t",), writes=(n,))
        if (word & 0xF0FF) == 0x0023:
            return I("braf", R(n), kind=JUMP_IND, delay=True, rn=n, reads=(n,))
        if (word & 0xF0FF) == 0x0003:
            return I("bsrf", R(n), kind=CALL_IND, delay=True, rn=n, reads=(n,), writes=("pr",))
        if (word & 0xF0FF) == 0x000A:
            return I("sts", f"mach,{R(n)}", rn=n, reads=("mach",), writes=(n,))
        if (word & 0xF0FF) == 0x001A:
            return I("sts", f"macl,{R(n)}", rn=n, reads=("macl",), writes=(n,))
        if (word & 0xF0FF) == 0x002A:
            return I("sts", f"pr,{R(n)}", rn=n, reads=("pr",), writes=(n,))
        if (word & 0xF0FF) == 0x0002:
            return I("stc", f"sr,{R(n)}", rn=n, reads=("sr",), writes=(n,))
        if (word & 0xF0FF) == 0x0012:
            return I("stc", f"gbr,{R(n)}", rn=n, reads=("gbr",), writes=(n,))
        if (word & 0xF0FF) == 0x0022:
            return I("stc", f"vbr,{R(n)}", rn=n, reads=("vbr",), writes=(n,))
        return I(".word", f"0x{word:04x}", kind=INVALID)

    # ---------------------------------------------------------------- 0x1
    if op == 0x1:
        return I("mov.l", f"{R(m)},@({d4 * 4},{R(n)})", rn=n, rm=m, imm=d4 * 4, reads=(m, n))

    # ---------------------------------------------------------------- 0x2
    if op == 0x2:
        t = {
            0x0: ("mov.b", f"{R(m)},@{R(n)}", (m, n), ()),
            0x1: ("mov.w", f"{R(m)},@{R(n)}", (m, n), ()),
            0x2: ("mov.l", f"{R(m)},@{R(n)}", (m, n), ()),
            0x4: ("mov.b", f"{R(m)},@-{R(n)}", (m, n), (n,)),
            0x5: ("mov.w", f"{R(m)},@-{R(n)}", (m, n), (n,)),
            0x6: ("mov.l", f"{R(m)},@-{R(n)}", (m, n), (n,)),
            0x7: ("div0s", f"{R(m)},{R(n)}", (m, n), ("t", "q", "m")),
            0x8: ("tst", f"{R(m)},{R(n)}", (m, n), ("t",)),
            0x9: ("and", f"{R(m)},{R(n)}", (m, n), (n,)),
            0xA: ("xor", f"{R(m)},{R(n)}", (m, n), (n,)),
            0xB: ("or", f"{R(m)},{R(n)}", (m, n), (n,)),
            0xC: ("cmp/str", f"{R(m)},{R(n)}", (m, n), ("t",)),
            0xD: ("xtrct", f"{R(m)},{R(n)}", (m, n), (n,)),
            0xE: ("mulu.w", f"{R(m)},{R(n)}", (m, n), ("macl",)),
            0xF: ("muls.w", f"{R(m)},{R(n)}", (m, n), ("macl",)),
        }.get(d4)
        if t:
            return I(t[0], t[1], rn=n, rm=m, reads=t[2], writes=t[3])
        return I(".word", f"0x{word:04x}", kind=INVALID)

    # ---------------------------------------------------------------- 0x3
    if op == 0x3:
        t = {
            0x0: ("cmp/eq", ("t",)),
            0x2: ("cmp/hs", ("t",)),
            0x3: ("cmp/ge", ("t",)),
            0x4: ("div1", (n, "t", "q", "m")),
            0x5: ("dmulu.l", ("mach", "macl")),
            0x6: ("cmp/hi", ("t",)),
            0x7: ("cmp/gt", ("t",)),
            0x8: ("sub", (n,)),
            0xA: ("subc", (n, "t")),
            0xB: ("subv", (n, "t")),
            0xC: ("add", (n,)),
            0xD: ("dmuls.l", ("mach", "macl")),
            0xE: ("addc", (n, "t")),
            0xF: ("addv", (n, "t")),
        }.get(d4)
        if t:
            rd = (m, n) + (("t",) if t[0] in ("subc", "addc", "div1") else ())
            return I(t[0], f"{R(m)},{R(n)}", rn=n, rm=m, reads=rd, writes=t[1])
        return I(".word", f"0x{word:04x}", kind=INVALID)

    # ---------------------------------------------------------------- 0x4
    if op == 0x4:
        lo = word & 0xFF
        # mac.w @Rm+,@Rn+ is the only 0x4 encoding with a live m field.
        if d4 == 0xF:
            return I("mac.w", f"@{R(m)}+,@{R(n)}+", rn=n, rm=m,
                     reads=(m, n), writes=(m, n, "mach", "macl"))
        one_reg = {
            0x00: ("shll", (n,), (n, "t")),
            0x01: ("shlr", (n,), (n, "t")),
            0x04: ("rotl", (n,), (n, "t")),
            0x05: ("rotr", (n,), (n, "t")),
            0x08: ("shll2", (n,), (n,)),
            0x09: ("shlr2", (n,), (n,)),
            0x10: ("dt", (n,), (n, "t")),
            0x11: ("cmp/pz", (n,), ("t",)),
            0x15: ("cmp/pl", (n,), ("t",)),
            0x18: ("shll8", (n,), (n,)),
            0x19: ("shlr8", (n,), (n,)),
            0x20: ("shal", (n,), (n, "t")),
            0x21: ("shar", (n,), (n, "t")),
            0x24: ("rotcl", (n, "t"), (n, "t")),
            0x25: ("rotcr", (n, "t"), (n, "t")),
            0x28: ("shll16", (n,), (n,)),
            0x29: ("shlr16", (n,), (n,)),
        }.get(lo)
        if one_reg:
            return I(one_reg[0], R(n), rn=n, reads=one_reg[1], writes=one_reg[2])
        # store control/system register, pre-decrement
        st_dec = {0x02: "mach", 0x12: "macl", 0x22: "pr",
                  0x03: "sr", 0x13: "gbr", 0x23: "vbr"}.get(lo)
        if st_dec:
            mn = "stc.l" if lo in (0x03, 0x13, 0x23) else "sts.l"
            return I(mn, f"{st_dec},@-{R(n)}", rn=n, reads=(n, st_dec), writes=(n,))
        # Load control/system register. The manual names the source Rm, but it
        # is encoded in bits 11-8 — the same field position as Rn elsewhere.
        ld_inc = {0x06: "mach", 0x16: "macl", 0x26: "pr",
                  0x07: "sr", 0x17: "gbr", 0x27: "vbr"}.get(lo)
        if ld_inc:
            mn = "ldc.l" if lo in (0x07, 0x17, 0x27) else "lds.l"
            return I(mn, f"@{R(n)}+,{ld_inc}", rn=n, reads=(n,), writes=(n, ld_inc))
        ld = {0x0A: "mach", 0x1A: "macl", 0x2A: "pr",
              0x0E: "sr", 0x1E: "gbr", 0x2E: "vbr"}.get(lo)
        if ld:
            mn = "ldc" if lo in (0x0E, 0x1E, 0x2E) else "lds"
            return I(mn, f"{R(n)},{ld}", rn=n, reads=(n,), writes=(ld,))
        if lo == 0x0B:
            return I("jsr", f"@{R(n)}", kind=CALL_IND, delay=True, rn=n,
                     reads=(n,), writes=("pr",))
        if lo == 0x2B:
            return I("jmp", f"@{R(n)}", kind=JUMP_IND, delay=True, rn=n, reads=(n,))
        if lo == 0x1B:
            return I("tas.b", f"@{R(n)}", rn=n, reads=(n,), writes=("t",))
        if lo == 0x0F:
            return I("mac.w", f"@{R(m)}+,@{R(n)}+", rn=n, rm=m,
                     reads=(m, n), writes=(m, n, "mach", "macl"))
        return I(".word", f"0x{word:04x}", kind=INVALID)

    # ---------------------------------------------------------------- 0x5
    if op == 0x5:
        return I("mov.l", f"@({d4 * 4},{R(m)}),{R(n)}", rn=n, rm=m, imm=d4 * 4,
                 reads=(m,), writes=(n,))

    # ---------------------------------------------------------------- 0x6
    if op == 0x6:
        t = {
            0x0: ("mov.b", f"@{R(m)},{R(n)}", (m,), (n,)),
            0x1: ("mov.w", f"@{R(m)},{R(n)}", (m,), (n,)),
            0x2: ("mov.l", f"@{R(m)},{R(n)}", (m,), (n,)),
            0x3: ("mov", f"{R(m)},{R(n)}", (m,), (n,)),
            0x4: ("mov.b", f"@{R(m)}+,{R(n)}", (m,), (n, m)),
            0x5: ("mov.w", f"@{R(m)}+,{R(n)}", (m,), (n, m)),
            0x6: ("mov.l", f"@{R(m)}+,{R(n)}", (m,), (n, m)),
            0x7: ("not", f"{R(m)},{R(n)}", (m,), (n,)),
            0x8: ("swap.b", f"{R(m)},{R(n)}", (m,), (n,)),
            0x9: ("swap.w", f"{R(m)},{R(n)}", (m,), (n,)),
            0xA: ("negc", f"{R(m)},{R(n)}", (m, "t"), (n, "t")),
            0xB: ("neg", f"{R(m)},{R(n)}", (m,), (n,)),
            0xC: ("extu.b", f"{R(m)},{R(n)}", (m,), (n,)),
            0xD: ("extu.w", f"{R(m)},{R(n)}", (m,), (n,)),
            0xE: ("exts.b", f"{R(m)},{R(n)}", (m,), (n,)),
            0xF: ("exts.w", f"{R(m)},{R(n)}", (m,), (n,)),
        }[d4]
        return I(t[0], t[1], rn=n, rm=m, reads=t[2], writes=t[3])

    # ---------------------------------------------------------------- 0x7
    if op == 0x7:
        v = _simm8(d8)
        return I("add", f"#{v},{R(n)}", rn=n, imm=v, reads=(n,), writes=(n,))

    # ---------------------------------------------------------------- 0x8
    if op == 0x8:
        sub = (word >> 8) & 0xF
        if sub == 0x0:
            return I("mov.b", f"r0,@({d4},{R(m)})", rn=m, imm=d4, reads=(0, m))
        if sub == 0x1:
            return I("mov.w", f"r0,@({d4 * 2},{R(m)})", rn=m, imm=d4 * 2, reads=(0, m))
        if sub == 0x4:
            return I("mov.b", f"@({d4},{R(m)}),r0", rm=m, imm=d4, reads=(m,), writes=(0,))
        if sub == 0x5:
            return I("mov.w", f"@({d4 * 2},{R(m)}),r0", rm=m, imm=d4 * 2, reads=(m,), writes=(0,))
        if sub == 0x8:
            v = _simm8(d8)
            return I("cmp/eq", f"#{v},r0", imm=v, reads=(0,), writes=("t",))
        # conditional branches: target = PC + 4 + disp*2
        tgt = addr + 4 + _s8(d8) * 2
        if sub == 0x9:
            return I("bt", f"0x{tgt:x}", kind=BRANCH, target=tgt, reads=("t",))
        if sub == 0xB:
            return I("bf", f"0x{tgt:x}", kind=BRANCH, target=tgt, reads=("t",))
        if sub == 0xD:
            return I("bt.s", f"0x{tgt:x}", kind=BRANCH, target=tgt, delay=True, reads=("t",))
        if sub == 0xF:
            return I("bf.s", f"0x{tgt:x}", kind=BRANCH, target=tgt, delay=True, reads=("t",))
        return I(".word", f"0x{word:04x}", kind=INVALID)

    # ---------------------------------------------------------------- 0x9  mov.w @(disp,pc)
    if op == 0x9:
        lit = addr + 4 + d8 * 2
        return I("mov.w", f"@({d8 * 2},pc),{R(n)}", rn=n, pool=(lit, 2), writes=(n,))

    # ---------------------------------------------------------------- 0xA/0xB
    if op == 0xA:
        tgt = addr + 4 + _s12(d12) * 2
        return I("bra", f"0x{tgt:x}", kind=JUMP, target=tgt, delay=True)
    if op == 0xB:
        tgt = addr + 4 + _s12(d12) * 2
        return I("bsr", f"0x{tgt:x}", kind=CALL, target=tgt, delay=True, writes=("pr",))

    # ---------------------------------------------------------------- 0xC
    if op == 0xC:
        sub = (word >> 8) & 0xF
        gbr = {
            0x0: ("mov.b", f"r0,@({d8},gbr)"), 0x1: ("mov.w", f"r0,@({d8 * 2},gbr)"),
            0x2: ("mov.l", f"r0,@({d8 * 4},gbr)"),
            0x4: ("mov.b", f"@({d8},gbr),r0"), 0x5: ("mov.w", f"@({d8 * 2},gbr),r0"),
            0x6: ("mov.l", f"@({d8 * 4},gbr),r0"),
        }.get(sub)
        if gbr:
            store = sub in (0x0, 0x1, 0x2)
            return I(gbr[0], gbr[1], imm=d8, reads=("gbr",) + ((0,) if store else ()),
                     writes=() if store else (0,))
        if sub == 0x3:
            return I("trapa", f"#{d8}", kind=JUMP_IND)
        if sub == 0x7:
            # mova: PC is aligned down to 4 before adding
            lit = ((addr + 4) & ~3) + d8 * 4
            return I("mova", f"@({d8 * 4},pc),r0", imm=lit, writes=(0,))
        t = {
            0x8: ("tst", f"#{d8},r0", (0,), ("t",)),
            0x9: ("and", f"#{d8},r0", (0,), (0,)),
            0xA: ("xor", f"#{d8},r0", (0,), (0,)),
            0xB: ("or", f"#{d8},r0", (0,), (0,)),
            0xC: ("tst.b", f"#{d8},@(r0,gbr)", (0, "gbr"), ("t",)),
            0xD: ("and.b", f"#{d8},@(r0,gbr)", (0, "gbr"), ()),
            0xE: ("xor.b", f"#{d8},@(r0,gbr)", (0, "gbr"), ()),
            0xF: ("or.b", f"#{d8},@(r0,gbr)", (0, "gbr"), ()),
        }[sub]
        return I(t[0], t[1], imm=d8, reads=t[2], writes=t[3])

    # ---------------------------------------------------------------- 0xD  mov.l @(disp,pc)
    if op == 0xD:
        lit = ((addr + 4) & ~3) + d8 * 4
        return I("mov.l", f"@({d8 * 4},pc),{R(n)}", rn=n, pool=(lit, 4), writes=(n,))

    # ---------------------------------------------------------------- 0xE
    if op == 0xE:
        v = _simm8(d8)
        return I("mov", f"#{v},{R(n)}", rn=n, imm=v, writes=(n,))

    # 0xF is the SH-2E/SH-3 FPU space; plain SH-2 has no encodings here.
    return I(".word", f"0x{word:04x}", kind=INVALID)


def decode_stream(data: bytes, base: int, start: int = 0, count: Optional[int] = None):
    """Decode `count` instructions from `data`, where data[0] lives at `base`."""
    end = len(data) if count is None else min(len(data), start + count * 2)
    for off in range(start, end - 1, 2):
        yield decode(int.from_bytes(data[off:off + 2], "big"), base + off)
