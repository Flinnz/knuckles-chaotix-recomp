"""Motorola 68000 instruction decoder.

Unlike the SH-2, 68000 instructions are variable length: a 16-bit opcode word
followed by 0-4 extension words whose count depends on the operand sizes *and*
on the addressing modes encoded in the opcode. Getting that length exactly right
is the property everything else rests on — one wrong length desynchronises the
entire instruction stream from that point on.

Text output follows GNU `m68k-elf-objdump`'s MIT syntax so it can be diffed
against it: `%a0@(8,%d1:w)`, size suffix fused onto the mnemonic (`movel`),
PC-relative operands resolved to absolute addresses, immediates as signed
decimal, and `%fp`/`%sp` for a6/a7.
"""

from dataclasses import dataclass
from typing import Optional

NORMAL = "normal"
BRANCH = "branch"
JUMP = "jump"
JUMP_IND = "jump_ind"
CALL = "call"
CALL_IND = "call_ind"
RET = "ret"
TRAP = "trap"
INVALID = "invalid"

COND = ["t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
        "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"]
SIZE_SUF = {1: "b", 2: "w", 4: "l"}


@dataclass
class EA:
    """One effective address in machine terms rather than as text.

    The text form is what objdump can be diffed against; this is what a code
    generator needs, and both come out of the same parse so they cannot drift.
    Populated in decode order, so `Insn.eas[0]` is the source of a two-operand
    instruction and `eas[1]` its destination.
    """
    mode: int
    reg: int
    text: str
    disp: int = 0                     # (d16,An), (d8,An,Xn), (d16,PC)
    index: Optional[tuple] = None     # (is_areg, reg, is_long, scale)
    addr: Optional[int] = None        # absolute, or a resolved PC-relative one
    imm: Optional[int] = None         # immediate, signed


@dataclass
class Insn:
    addr: int
    size: int = 2                  # total length in bytes
    mnem: str = ".short"
    ops: str = ""
    kind: str = NORMAL
    target: Optional[int] = None
    opsize: Optional[int] = None   # operand size in bytes
    word: int = 0
    # True when the original bytes cannot be reproduced from this instruction's
    # assembly text — see Cursor.lossy.
    lossy: bool = False
    eas: tuple = ()                # decoded effective addresses, in order
    # The immediate an instruction carries in its own extension words rather
    # than in an effective address: `andi #x,ea`, a static bit number, a `movem`
    # register mask, `link`'s frame size, `trap`'s vector.
    imm: Optional[int] = None

    def text(self):
        return f"{self.mnem} {self.ops}" if self.ops else self.mnem

    def __str__(self):
        return f"{self.addr:08x}: {self.text()}"


def areg(n):
    return {6: "%fp", 7: "%sp"}[n] if n >= 6 else f"%a{n}"


def dreg(n):
    return f"%d{n}"


def _s8(v):
    return v - 0x100 if v & 0x80 else v


def _s16(v):
    return v - 0x10000 if v & 0x8000 else v


def _s32(v):
    return v - 0x100000000 if v & 0x80000000 else v


def _signed(v, nbytes):
    return {1: _s8, 2: _s16, 4: _s32}[nbytes](v)


class Cursor:
    """Sequential extension-word reader that also tracks total length."""

    def __init__(self, fetch, addr):
        self.fetch = fetch
        self.base = addr
        self.pos = addr + 2      # first extension word
        # A byte immediate occupies only the low half of its extension word.
        # The hardware ignores the high half, and this cartridge leaves nonzero
        # junk there, which no assembler syntax can express — so such an
        # instruction has to be emitted as raw data to stay byte-exact.
        self.lossy = False
        self.eas = []            # structured operands, for the code generator
        self.imm = None          # an immediate the instruction carries itself

    def word(self):
        v = self.fetch(self.pos)
        self.pos += 2
        return v

    def long(self):
        hi = self.word()
        return (hi << 16) | self.word()

    @property
    def length(self):
        return self.pos - self.base


def brief_ext(w, base_text, disp_origin=None, emit=False):
    """Decode a brief extension word: %an@(d8,%xn:s) or %pc@(target,%xn:s).

    objdump prints this displacement as bare hex — sign-extended to 64 bits when
    negative — unlike the 16-bit displacement form, which it prints as signed
    decimal. The `:2`/`:4`/`:8` scale suffix is a 68020 field that a 68000
    ignores, but objdump still renders it, so it is reproduced here.
    """
    ireg = (w >> 12) & 7
    is_addr = w & 0x8000
    long_idx = w & 0x0800
    scale = (w >> 9) & 3
    disp = _s8(w & 0xFF)
    idx = f"{areg(ireg) if is_addr else dreg(ireg)}:{'l' if long_idx else 'w'}"
    if scale:
        idx += f":{1 << scale}"
    if disp_origin is not None:
        return f"%pc@(0x{(disp_origin + disp) & 0xFFFFFFFF:x},{idx})"
    if emit:
        # gas reads an unprefixed number as decimal, so the objdump-matching
        # hex form would assemble to a different displacement entirely.
        return f"{base_text}@({disp},{idx})"
    return f"{base_text}@({disp & 0xFFFFFFFFFFFFFFFF if disp < 0 else disp:x},{idx})"


def reg_ea(cur, is_areg, n, at=None):
    """Record a register operand that the text builds directly.

    Half the two-operand encodings name one side in the opcode word rather than
    through an effective address — `add.w <ea>,%d3` is one EA and a register
    field — so without this `Insn.eas` would hold only half the operands and a
    code generator would have to know which half from the mnemonic. `at` places
    it, since the register can be either the source or the destination.
    """
    e = EA(mode=1 if is_areg else 0, reg=n, text=areg(n) if is_areg else dreg(n))
    cur.eas.append(e) if at is None else cur.eas.insert(at, e)
    return e.text


def mem_ea(cur, mode, reg, at=None):
    """The same for `-(An)`/`(An)+` pairs written as text: addx, subx, cmpm."""
    e = EA(mode=mode, reg=reg,
           text=f"{areg(reg)}@" + ("+" if mode == 3 else "-" if mode == 4 else ""))
    cur.eas.append(e) if at is None else cur.eas.insert(at, e)
    return e.text


def ea_decode(mode, reg, opsize, cur, emit=False):
    """Render one effective address, consuming its extension words.

    With `emit` set, absolute operands carry an explicit `:w`/`:l` size suffix.
    objdump prints both forms as a bare hex number, but the assembler picks the
    size from the value, so a bare number would silently re-encode a long
    absolute as a short one and break the round-trip.

    Returns (text, is_pcrel_target_or_None), and records the same address in
    machine terms on `cur.eas` for the code generator.
    """
    def keep(text, **kw):
        cur.eas.append(EA(mode=mode, reg=reg, text=text, **kw))
        return text

    def idx_of(w):
        return ((w & 0x8000) != 0, (w >> 12) & 7, (w & 0x0800) != 0,
                1 << ((w >> 9) & 3))

    if mode in (0, 1):
        return keep(dreg(reg) if mode == 0 else areg(reg)), None
    if mode in (2, 3, 4):
        return keep(f"{areg(reg)}@" + ("+" if mode == 3 else "-" if mode == 4 else "")), None
    if mode == 5:
        d = _s16(cur.word())
        # gas rewrites a zero displacement into plain (An), which is two bytes
        # shorter. The `:w` suffix pins the (d16,An) form the cartridge uses.
        t = f"{areg(reg)}@(0:w)" if emit and d == 0 else f"{areg(reg)}@({d})"
        return keep(t, disp=d), None
    if mode == 6:
        x = cur.word()
        return keep(brief_ext(x, areg(reg), emit=emit),
                    disp=_s8(x & 0xFF), index=idx_of(x)), None
    if mode == 7:
        if reg == 0:
            a = _s16(cur.word()) & 0xFFFFFFFF
            return keep(f"0x{a:x}" + (":w" if emit else ""), addr=a), None
        if reg == 1:
            a = cur.long()
            return keep(f"0x{a:x}" + (":l" if emit else ""), addr=a), None
        if reg == 2:
            origin = cur.pos
            d = _s16(cur.word())
            t = (origin + d) & 0xFFFFFFFF
            return keep(f"%pc@(0x{t:x})", disp=d, addr=t), t
        if reg == 3:
            origin = cur.pos
            x = cur.word()
            return keep(brief_ext(x, None, disp_origin=origin, emit=emit),
                        disp=_s8(x & 0xFF), index=idx_of(x),
                        addr=(origin + _s8(x & 0xFF)) & 0xFFFFFFFF), None
        if reg == 4:
            if opsize == 4:
                v = _s32(cur.long())
                raw = v
            elif opsize == 1:
                # A byte immediate occupies only the low half of its extension
                # word. objdump prints that byte signed; emitting it signed
                # would make gas sign-extend across the unused high half and
                # change the encoded word, so emit mode keeps it unsigned.
                ext = cur.word()
                raw = _s8(ext & 0xFF)
                if ext >> 8:
                    cur.lossy = True
                v = (ext & 0xFF) if emit else raw
            else:
                v = raw = _s16(cur.word())
            return keep(f"#{v}", imm=raw), None
    return "<bad>", None


def ea_valid(mode, reg):
    return mode != 7 or reg <= 4


# The 68000 restricts each instruction to a *class* of addressing modes, and
# encodings outside that class are illegal rather than merely unusual. Without
# these checks a decoder happily invents instructions out of data, which is
# exactly what desynchronises a disassembly.
def is_data(mode, reg):
    """Anything but an address register."""
    return mode != 1


def is_memory(mode, reg):
    return mode not in (0, 1)


def is_alterable(mode, reg):
    """Writable: excludes PC-relative and immediate."""
    return not (mode == 7 and reg in (2, 3, 4))


def is_control(mode, reg):
    """Addresses a memory location without predecrement/postincrement."""
    return mode in (2, 5, 6) or (mode == 7 and reg in (0, 1, 2, 3))


def is_data_alt(mode, reg):
    return ea_valid(mode, reg) and is_data(mode, reg) and is_alterable(mode, reg)


def is_mem_alt(mode, reg):
    return ea_valid(mode, reg) and is_memory(mode, reg) and is_alterable(mode, reg)


def reglist_text(mask, predec):
    """Render a MOVEM register mask the way objdump does.

    In predecrement mode the mask bit order is reversed. Contiguous runs across
    the d0..d7,a0..a7 sequence are compressed, which is why a full save prints
    as `%d0-%fp` rather than two separate ranges.
    """
    names = [dreg(i) for i in range(8)] + [areg(i) for i in range(8)]
    present = []
    for i in range(16):
        bit = (15 - i) if predec else i
        if mask & (1 << bit):
            present.append(i)
    if not present:
        return ""
    parts, run_start, prev = [], present[0], present[0]
    for i in present[1:]:
        if i == prev + 1:
            prev = i
            continue
        parts.append((run_start, prev))
        run_start = prev = i
    parts.append((run_start, prev))
    return "/".join(names[a] if a == b else f"{names[a]}-{names[b]}"
                    for a, b in parts)


def decode(fetch, addr, emit=False):
    """Decode the instruction at `addr`; `fetch(a)` returns the 16-bit word."""
    w = fetch(addr)
    cur = Cursor(fetch, addr)
    op = (w >> 12) & 0xF
    mode = (w >> 3) & 7
    reg = w & 7
    rx = (w >> 9) & 7

    def done(mnem, ops="", kind=NORMAL, target=None, opsize=None):
        return Insn(addr=addr, size=cur.length, mnem=mnem, ops=ops, kind=kind,
                    target=target, opsize=opsize, word=w, lossy=cur.lossy,
                    eas=tuple(cur.eas), imm=cur.imm)

    def bad():
        return Insn(addr=addr, size=2, mnem=".short", ops=f"0x{w:04x}",
                    kind=INVALID, word=w)

    def sized(base, sz):
        return base + SIZE_SUF[sz]

    # ---------------------------------------------------------------- 0x0
    if op == 0x0:
        szf = (w >> 6) & 3
        if w & 0x0100:                                  # dynamic bit ops / movep
            name = ["btst", "bchg", "bclr", "bset"][szf]
            if mode == 1:                               # movep
                d = _s16(cur.word())
                mn = "movep" + ("l" if w & 0x0040 else "w")
                if w & 0x0080:
                    return done(mn, f"{dreg(rx)},{areg(reg)}@({d})")
                return done(mn, f"{areg(reg)}@({d}),{dreg(rx)}")
            ok = (ea_valid(mode, reg) and is_data(mode, reg)) if szf == 0 \
                else is_data_alt(mode, reg)
            if not ok:
                return bad()
            ea, _ = ea_decode(mode, reg, 1, cur, emit)
            return done(name, f"{reg_ea(cur, False, rx, 0)},{ea}")
        top = (w >> 8) & 0xF
        if top in (0x0, 0x2, 0x4, 0x6, 0xA, 0xC):
            name = {0x0: "ori", 0x2: "andi", 0x4: "subi", 0x6: "addi",
                    0xA: "eori", 0xC: "cmpi"}[top]
            if szf == 3:
                return bad()
            sz = (1, 2, 4)[szf]
            # ORI/ANDI/EORI to CCR (#imm,%ccr) and to SR (#imm,%sr)
            # The immediate is in the extension word, and it has to be *kept*:
            # these two forms are the only ones here that carried theirs in the
            # operand text alone, so the recompiler had nothing to read and took
            # the low byte of the opcode word instead — 0x3C, the "to CCR"
            # selector, in place of whatever was written. `ori #1,ccr` before an
            # `rts` returns a flag in C, and what ran set X, N and Z and cleared
            # C, which is the opposite answer. The listing round-trip could not
            # see it because the text was right all along.
            if w & 0xFF == 0x3C and name in ("ori", "andi", "eori"):
                raw = cur.word() & 0xFF
                cur.imm = _s8(raw)
                return done(name + "b", f"#{raw if emit else _s8(raw)},%ccr")
            if w & 0xFF == 0x7C and name in ("ori", "andi", "eori"):
                cur.imm = _s16(cur.word())
                return done(name + "w", f"#{cur.imm},%sr")
            if not is_data_alt(mode, reg):
                return bad()
            # Same rule as an immediate EA: the byte form uses only the low
            # half of its extension word, so emit it unsigned.
            if sz == 4:
                imm = cur.imm = _s32(cur.long())
            elif sz == 1:
                ext = cur.word()
                raw = ext & 0xFF
                if ext >> 8:
                    cur.lossy = True
                cur.imm = _s8(raw)
                imm = raw if emit else _s8(raw)
            else:
                imm = cur.imm = _s16(cur.word())
            ea, _ = ea_decode(mode, reg, sz, cur, emit)
            return done(sized(name, sz), f"#{imm},{ea}", opsize=sz)
        if top == 0x8:                                  # static bit ops
            name = ["btst", "bchg", "bclr", "bset"][szf]
            ok = (ea_valid(mode, reg) and is_data(mode, reg)
                  and not (mode == 7 and reg == 4)) if szf == 0 \
                else is_data_alt(mode, reg)
            if not ok:
                return bad()
            ext = cur.word()
            bit = cur.imm = ext & 0xFF
            if ext >> 8:
                cur.lossy = True
            ea, _ = ea_decode(mode, reg, 1, cur, emit)
            return done(name, f"#{bit},{ea}")
        return bad()

    # ---------------------------------------------------------- 0x1/2/3 move
    if op in (0x1, 0x2, 0x3):
        sz = {1: 1, 3: 2, 2: 4}[op]
        dmode = (w >> 6) & 7
        dreg_ = rx
        if not ea_valid(mode, reg) or not ea_valid(dmode, dreg_):
            return bad()
        if sz == 1 and (mode == 1 or dmode == 1):
            return bad()               # no movea.b, and An is not byte-addressable
        if dmode != 1 and not is_data_alt(dmode, dreg_):
            return bad()
        src, _ = ea_decode(mode, reg, sz, cur, emit)
        if dmode == 1:
            return done(sized("movea", sz),
                        f"{src},{reg_ea(cur, True, dreg_)}", opsize=sz)
        dst, _ = ea_decode(dmode, dreg_, sz, cur, emit)
        return done(sized("move", sz), f"{src},{dst}", opsize=sz)

    # ---------------------------------------------------------------- 0x4
    if op == 0x4:
        szf = (w >> 6) & 3
        if w == 0x4AFC:
            return done("illegal", kind=TRAP)
        if (w & 0xFFC0) == 0x40C0:
            if not is_data_alt(mode, reg):
                return bad()
            ea, _ = ea_decode(mode, reg, 2, cur, emit)
            return done("movew", f"%sr,{ea}")
        if (w & 0xFFC0) == 0x44C0:
            if not (ea_valid(mode, reg) and is_data(mode, reg)):
                return bad()
            ea, _ = ea_decode(mode, reg, 2, cur, emit)
            return done("movew", f"{ea},%ccr")
        if (w & 0xFFC0) == 0x46C0:
            if not (ea_valid(mode, reg) and is_data(mode, reg)):
                return bad()
            ea, _ = ea_decode(mode, reg, 2, cur, emit)
            return done("movew", f"{ea},%sr")
        if (w & 0xFFC0) == 0x4AC0:
            if not is_data_alt(mode, reg):
                return bad()
            ea, _ = ea_decode(mode, reg, 1, cur, emit)
            return done("tas", ea)
        if (w & 0xFF00) in (0x4000, 0x4200, 0x4400, 0x4600, 0x4A00):
            name = {0x4000: "negx", 0x4200: "clr", 0x4400: "neg",
                    0x4600: "not", 0x4A00: "tst"}[w & 0xFF00]
            if szf == 3:
                return bad()
            if not is_data_alt(mode, reg):
                return bad()
            sz = (1, 2, 4)[szf]
            ea, _ = ea_decode(mode, reg, sz, cur, emit)
            return done(sized(name, sz), ea, opsize=sz)
        if (w & 0xFFC0) == 0x4AC0:
            if not is_data_alt(mode, reg):
                return bad()
            ea, _ = ea_decode(mode, reg, 1, cur, emit)
            return done("tas", ea)
        if (w & 0xFFF8) == 0x4840:
            return done("swap", dreg(reg))
        if (w & 0xFFC0) == 0x4840:
            if not is_control(mode, reg):
                return bad()
            ea, _ = ea_decode(mode, reg, 4, cur, emit)
            return done("pea", ea)
        if (w & 0xFFF8) == 0x4880:
            return done("extw", dreg(reg))
        if (w & 0xFFF8) == 0x48C0:
            return done("extl", dreg(reg))
        if (w & 0xFFC0) == 0x4800:
            if not is_data_alt(mode, reg):
                return bad()
            ea, _ = ea_decode(mode, reg, 1, cur, emit)
            return done("nbcd", ea)
        if (w & 0xFB80) == 0x4880:                      # movem
            sz = 4 if w & 0x0040 else 2
            to_mem = not (w & 0x0400)
            mask = cur.imm = cur.word()
            # to memory: control-alterable or -(An); from memory: control or (An)+
            ok = (is_control(mode, reg) and is_alterable(mode, reg)) or mode == 4 \
                if to_mem else (is_control(mode, reg) or mode == 3)
            if not ok:
                return bad()
            ea, _ = ea_decode(mode, reg, sz, cur, emit)
            rl = reglist_text(mask, predec=(mode == 4))
            return done(sized("movem", sz),
                        f"{rl},{ea}" if to_mem else f"{ea},{rl}", opsize=sz)
        if (w & 0xFFF0) == 0x4E40:
            return done("trap", f"#{w & 0xF}", kind=TRAP)
        if (w & 0xFFF8) == 0x4E50:
            cur.imm = _s16(cur.word())
            return done("linkw", f"{areg(reg)},#{cur.imm}")
        if (w & 0xFFF8) == 0x4E58:
            return done("unlk", areg(reg))
        if (w & 0xFFF8) == 0x4E60:
            return done("movel", f"{areg(reg)},%usp")
        if (w & 0xFFF8) == 0x4E68:
            return done("movel", f"%usp,{areg(reg)}")
        if w == 0x4E70:
            return done("reset")
        if w == 0x4E71:
            return done("nop")
        if w == 0x4E72:
            return done("stop", f"#{_s16(cur.word())}", kind=RET)
        if w == 0x4E73:
            return done("rte", kind=RET)
        if w == 0x4E75:
            return done("rts", kind=RET)
        if w == 0x4E76:
            return done("trapv", kind=TRAP)
        if w == 0x4E77:
            return done("rtr", kind=RET)
        if (w & 0xFFC0) == 0x4E80 or (w & 0xFFC0) == 0x4EC0:
            is_jsr = (w & 0x0040) == 0
            if not is_control(mode, reg):
                return bad()
            ea, pct = ea_decode(mode, reg, 4, cur, emit)
            tgt = None
            if mode == 7 and reg in (0, 1):
                tgt = int(ea.split(":")[0], 16)     # emit mode appends :w / :l
            elif pct is not None:
                tgt = pct
            kind = (CALL if is_jsr else JUMP) if tgt is not None else \
                   (CALL_IND if is_jsr else JUMP_IND)
            return done("jsr" if is_jsr else "jmp", ea, kind=kind, target=tgt)
        if (w & 0xF1C0) == 0x41C0:                      # lea
            if not is_control(mode, reg):
                return bad()
            ea, _ = ea_decode(mode, reg, 4, cur, emit)
            return done("lea", f"{ea},{areg(rx)}")
        if (w & 0xF1C0) == 0x4180:                      # chk.w
            if not (ea_valid(mode, reg) and is_data(mode, reg)):
                return bad()
            ea, _ = ea_decode(mode, reg, 2, cur, emit)
            return done("chkw", f"{ea},{reg_ea(cur, False, rx)}")
        return bad()

    # ---------------------------------------------------------------- 0x5
    if op == 0x5:
        szf = (w >> 6) & 3
        if szf == 3:
            cc = (w >> 8) & 0xF
            if mode == 1:                               # dbcc
                d = _s16(cur.word())
                t = (addr + 2 + d) & 0xFFFFFFFF
                name = f"db{COND[cc]}"
                return done(name, f"{dreg(reg)},0x{t:x}", kind=BRANCH, target=t)
            if not is_data_alt(mode, reg):
                return bad()
            ea, _ = ea_decode(mode, reg, 1, cur, emit)
            return done(f"s{COND[cc]}", ea)
        sz = (1, 2, 4)[szf]
        # An address register destination is word/long only. objdump decodes
        # the byte form anyway; we reject it, because accepting encodings the
        # hardware refuses invents phantom code during discovery.
        if not (ea_valid(mode, reg) and is_alterable(mode, reg)) or \
                (mode == 1 and sz == 1):
            return bad()
        data = (w >> 9) & 7
        data = 8 if data == 0 else data
        ea, _ = ea_decode(mode, reg, sz, cur, emit)
        name = "subq" if w & 0x0100 else "addq"
        return done(sized(name, sz), f"#{data},{ea}", opsize=sz)

    # ---------------------------------------------------------------- 0x6
    if op == 0x6:
        cc = (w >> 8) & 0xF
        d8 = w & 0xFF
        if d8 == 0:
            d = _s16(cur.word())
            suffix = "w"
        else:
            d = _s8(d8)
            suffix = "s"
        t = (addr + 2 + d) & 0xFFFFFFFF
        if cc == 0:
            return done("bra" + suffix, f"0x{t:x}", kind=JUMP, target=t)
        if cc == 1:
            return done("bsr" + suffix, f"0x{t:x}", kind=CALL, target=t)
        return done(f"b{COND[cc]}{suffix}", f"0x{t:x}", kind=BRANCH, target=t)

    # ---------------------------------------------------------------- 0x7
    if op == 0x7:
        if w & 0x0100:
            return bad()
        return done("moveq", f"#{_s8(w & 0xFF)},{dreg(rx)}", opsize=4)

    # ------------------------------------------------- 0x8/9/B/C/D arithmetic
    if op in (0x8, 0x9, 0xB, 0xC, 0xD):
        szf = (w >> 6) & 3
        direction = w & 0x0100
        base = {0x8: "or", 0x9: "sub", 0xB: "cmp", 0xC: "and", 0xD: "add"}[op]

        if szf == 3:                                    # <ea> to An, or div/mul
            if op in (0x9, 0xD, 0xB):
                sz = 4 if direction else 2
                if not ea_valid(mode, reg):
                    return bad()
                ea, _ = ea_decode(mode, reg, sz, cur, emit)
                nm = {0x9: "suba", 0xD: "adda", 0xB: "cmpa"}[op]
                return done(sized(nm, sz),
                            f"{ea},{reg_ea(cur, True, rx)}", opsize=sz)
            nm = {0x8: ("divu", "divs"), 0xC: ("mulu", "muls")}[op][1 if direction else 0]
            if not ea_valid(mode, reg) or mode == 1:
                return bad()
            ea, _ = ea_decode(mode, reg, 2, cur, emit)
            return done(nm + "w", f"{ea},{reg_ea(cur, False, rx)}")

        sz = (1, 2, 4)[szf]
        if direction:
            # This encoding space is shared: EXG, the BCD ops and the extend-add
            # forms all live here alongside "register to memory". EXG must be
            # tested first because it overlaps the ABCD register form.
            if op == 0xC:
                if (w & 0x01F8) == 0x0140:
                    return done("exg", f"{dreg(rx)},{dreg(reg)}")
                if (w & 0x01F8) == 0x0148:
                    return done("exg", f"{areg(rx)},{areg(reg)}")
                if (w & 0x01F8) == 0x0188:
                    return done("exg", f"{dreg(rx)},{areg(reg)}")
            if op in (0x8, 0xC) and szf == 0 and mode in (0, 1):
                nm = "sbcd" if op == 0x8 else "abcd"      # byte only
                if mode == 0:
                    return done(nm, f"{dreg(reg)},{dreg(rx)}")
                return done(nm, f"{areg(reg)}@-,{areg(rx)}@-")
            if op in (0x9, 0xD) and mode in (0, 1):
                nm = "subx" if op == 0x9 else "addx"
                if mode == 0:
                    return done(sized(nm, sz),
                                f"{reg_ea(cur, False, reg)},"
                                f"{reg_ea(cur, False, rx)}", opsize=sz)
                return done(sized(nm, sz),
                            f"{mem_ea(cur, 4, reg)},{mem_ea(cur, 4, rx)}",
                            opsize=sz)
            if op == 0xB and mode == 1:
                return done(sized("cmpm", sz),
                            f"{mem_ea(cur, 3, reg)},{mem_ea(cur, 3, rx)}",
                            opsize=sz)
            if op == 0xB:
                nm = "eor"                    # eor can target a data register
                if not is_data_alt(mode, reg):
                    return bad()
            else:
                nm = base                     # or/and/sub/add must reach memory
                if not is_mem_alt(mode, reg):
                    return bad()
            ea, _ = ea_decode(mode, reg, sz, cur, emit)
            return done(sized(nm, sz),
                        f"{reg_ea(cur, False, rx, 0)},{ea}", opsize=sz)

        if not ea_valid(mode, reg):
            return bad()
        if mode == 1 and (sz == 1 or op in (0x8, 0xC)):
            return bad()
        ea, _ = ea_decode(mode, reg, sz, cur, emit)
        return done(sized(base, sz),
                    f"{ea},{reg_ea(cur, False, rx)}", opsize=sz)

    # ---------------------------------------------------------------- 0xE
    if op == 0xE:
        szf = (w >> 6) & 3
        kinds = ["as", "ls", "rox", "ro"]
        d = "l" if w & 0x0100 else "r"
        if szf == 3:                                    # memory shift, word only
            if w & 0x0800:
                return bad()
            t = (w >> 9) & 3
            if not is_mem_alt(mode, reg):
                return bad()
            ea, _ = ea_decode(mode, reg, 2, cur, emit)
            return done(f"{kinds[t]}{d}w", ea, opsize=2)
        sz = (1, 2, 4)[szf]
        t = (w >> 3) & 3
        if w & 0x0020:
            return done(sized(kinds[t] + d, sz), f"{dreg(rx)},{dreg(reg)}", opsize=sz)
        cnt = rx if rx else 8
        return done(sized(kinds[t] + d, sz), f"#{cnt},{dreg(reg)}", opsize=sz)

    return bad()
