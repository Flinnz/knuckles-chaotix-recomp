"""Translate discovered 68000 functions into C.

One C function per discovered function, basic blocks as labels, registers in a
context struct — the same shape as `recomp/sh2c.py`, and different in three
places that matter.

**Nothing nests, again.** Every control transfer `return`s its destination and
`m68k_run` loops. On the SH-2 that was needed because handlers branch away
instead of returning; here it is needed because the 68000 keeps its return
address in memory, so `pea`/`rts` and `jsr` through a table entry are ordinary
things to do and none of them fit a C call. `jsr` pushes and returns the target;
`rts` returns what it pops.

**Effective addresses are computed once.** A destination like `(a1)+` is read,
modified and written, and its side effect happens exactly once — so an EA
becomes a `uint32_t` temporary holding its address, and the read and the write
both go through that. Predecrement adjusts before, postincrement after, and A7
moves by two for a byte so the stack stays even.

**Flags are set from the values, not the C result.** The 68000's V and C come
from sign relationships between source, destination and result, which C's own
arithmetic does not expose, so each operation passes all three to a helper in
src/m68000.h. Instructions that only set N and Z go through the logic helper,
which also clears V and C, and leaves X alone.
"""

from m68k.decode import (BRANCH, JUMP, JUMP_IND, CALL, CALL_IND, RET, TRAP,
                         INVALID, COND)


def fname(addr):
    return f"m_{addr:06X}"


def lname(addr):
    return f"L_{addr:06X}"


def D(n):
    return f"c->d[{n}]"


def A(n):
    return f"c->a[{n}]"


# Condition codes as C expressions. `hi`/`ls` are the unsigned pair, `ge`/`lt`
# the signed one; `t` and `f` exist because `dbra` is `dbf` and `bra`/`bsr`
# occupy the `bt`/`bf` slots in the branch encoding.
# Left unparenthesised so `if (...)` does not end up with two layers around a
# bare equality, which clang reads as a mistyped assignment and warns about.
CC = {
    "t": "1", "f": "0",
    "hi": "!c->c && !c->z", "ls": "c->c || c->z",
    "cc": "!c->c", "cs": "c->c",
    "ne": "!c->z", "eq": "c->z",
    "vc": "!c->v", "vs": "c->v",
    "pl": "!c->n", "mi": "c->n",
    "ge": "c->n == c->v", "lt": "c->n != c->v",
    "gt": "!c->z && c->n == c->v", "le": "c->z || c->n != c->v",
}

SUF = {1: "b", 2: "w", 4: "l"}
BITS = {1: 8, 2: 16, 4: 32}
UCAST = {1: "(uint8_t)", 2: "(uint16_t)", 4: "(uint32_t)"}
SCAST = {1: "(int32_t)(int8_t)", 2: "(int32_t)(int16_t)", 4: "(int32_t)"}


class Operand:
    """Where a decoded operand lives, once its address has been computed."""

    def __init__(self, kind, expr, size):
        self.kind = kind         # 'd', 'a', 'mem', 'imm'
        self.expr = expr
        self.size = size

    def read(self):
        if self.kind == "imm":
            return self.expr
        if self.kind == "mem":
            return f"M68K_R{BITS[self.size]}({self.expr})"
        if self.size == 4:
            return self.expr
        return f"({UCAST[self.size]}{self.expr})"

    def write(self, value):
        if self.kind == "mem":
            return f"M68K_W{BITS[self.size]}({self.expr}, {value});"
        if self.kind == "a" or self.size == 4:
            # Writing an address register is always a full 32-bit write, even
            # for a word operation: `movea.w` sign-extends into the whole
            # register, which is why the caller hands a sign-extended value.
            return f"{self.expr} = {value};"
        keep = 0xFFFFFFFF ^ ((1 << BITS[self.size]) - 1)
        return (f"{self.expr} = ({self.expr} & 0x{keep:08X}u) | "
                f"({UCAST[self.size]}({value}));")


SHIFTS = ("asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr")


class Codegen:
    def __init__(self, az, cycles):
        self.az = az
        self.cycles = cycles     # Musashi's 68000 table, one byte per opcode
        self.notes = []          # (addr, text) for anything outside the model

    # ------------------------------------------------------------------
    def cost(self, ins):
        """What one instruction costs in 68000 cycles.

        The base is Musashi's own table — `tools/m68kcycles.c` dumps it, and
        m68kmake has already expanded it per addressing mode, so the time an
        effective address takes is in the entry rather than needing a second
        table here. Taking it from the interpreter rather than from a reading of
        the manual is the argument `src/m68k_testmem.c` makes about semantics,
        applied to timing: the interpreter is the oracle, and `tools/refpoll.py`
        now says it is right against the reference in both of the engine's
        phases — 13.00 cycles for the comm poll where the reference measures
        12.97, 11.00 for the steady mix where it measures 11.00.

        What the table does not carry is what the handlers add while running.
        On a 68000 that is four things and they are all covered: a branch, whose
        cost depends on which edge it took, charged in `transfer()` where both
        edges are emitted; a shift by an immediate and a `movem`, whose count
        and register mask are in the instruction, added below; and a shift by a
        register, charged in `shift()` where the count exists.

        Two more are in Musashi and neither can run here. `mulu`/`muls` add two
        cycles a set bit of the operand and `scc` two when it sets — and across
        the reference's whole 68000 extract, 54,183 instructions, there is not
        one of either: 89.3% of what it executes is pure table cost and the rest
        is the four above. The fixed `USE_CYCLES(2)` and `(3)` in the generated
        opcodes belong to `moves`, `cas` and `cas2`, which are 68010 and 68020
        instructions this machine has no way to reach.
        """
        c = int(self.cycles[ins.word])
        m = ins.mnem
        base = m[:-1] if m and m[-1] in "bwl" else m
        w = ins.word
        if base in SHIFTS:
            # The memory form shifts one bit and the table already covers it;
            # the register form is counted at run time in `shift()`.
            if (w & 0xC0) != 0xC0 and not (w & 0x20):
                c += 2 * (((w >> 9) & 7) or 8)      # CYC_SHIFT is 1: two a bit
        elif base == "movem" and ins.imm is not None:
            # CYC_MOVEM_W and _L are shift counts, not multipliers: four cycles
            # a register for a word transfer and eight for a long one.
            c += bin(ins.imm & 0xFFFF).count("1") * (8 if m.endswith("l") else 4)
        return c

    def block_cycles(self, b):
        """What a block costs, up to and including anything that ends it early.

        An invalid opcode makes the emitter stop and hand the address back, so
        the instructions after it are never run and must not be charged.

        Never zero, and that floor is load-bearing rather than tidiness. The
        table has no entry for an opcode that is not an instruction, so a block
        that is nothing but an invalid word costs nothing — and such a block
        returns *its own address*, which the trampoline looks up and enters
        again. While the fuel was instructions every entry spent one and the
        slice ended; spending cycles, a free block is an infinite loop with no
        fuel between it and the frame. Two blocks in this cartridge are that
        shape, and one of them, 0x0283C2, is inside the address space the
        recompiled build can actually be sent to.
        """
        total = 0
        for ins in b.insns:
            total += self.cost(ins)
            if ins.kind == INVALID:
                break
        return max(total, 1)

    # ------------------------------------------------------------------
    def unhandled(self, ins, out):
        out.append(f"    /* UNHANDLED {ins.text()} */")
        self.notes.append((ins.addr, ins.text()))

    def ea(self, e, size, out, tag):
        """Turn one decoded effective address into an Operand.

        Emits whatever address arithmetic it needs first, so the side effects of
        `-(An)` and `(An)+` happen once and in the right order relative to the
        access. The name is tagged because an instruction can have two.
        """
        m, r = e.mode, e.reg
        if m == 0:
            return Operand("d", D(r), size)
        if m == 1:
            return Operand("a", A(r), size)
        v = f"ea{tag}_{e.mode}{e.reg}"
        if m == 2:
            out.append(f"    uint32_t {v} = {A(r)};")
        elif m == 3:
            step = 2 if (r == 7 and size == 1) else size
            out.append(f"    uint32_t {v} = {A(r)}; {A(r)} += {step};")
        elif m == 4:
            step = 2 if (r == 7 and size == 1) else size
            out.append(f"    {A(r)} -= {step}; uint32_t {v} = {A(r)};")
        elif m == 5:
            out.append(f"    uint32_t {v} = {A(r)} + {e.disp};")
        elif m == 6:
            out.append(f"    uint32_t {v} = {A(r)} + {e.disp} + {self.index(e)};")
        elif m == 7 and r in (0, 1):
            out.append(f"    uint32_t {v} = 0x{e.addr:08X}u;")
        elif m == 7 and r == 2:
            out.append(f"    uint32_t {v} = 0x{e.addr:08X}u;")
        elif m == 7 and r == 3:
            out.append(f"    uint32_t {v} = 0x{e.addr:08X}u + {self.index(e)};")
        elif m == 7 and r == 4:
            return Operand("imm", f"0x{e.imm & 0xFFFFFFFF:08X}u", size)
        else:
            return None
        return Operand("mem", v, size)

    def index(self, e):
        """The index register of a brief extension word.

        The scale field is 68020 and a 68000 ignores it, so it is not applied —
        and `looks_like_code` rejects anything that sets it, so nothing that
        reaches here has one.
        """
        is_areg, reg, is_long, _scale = e.index
        base = A(reg) if is_areg else D(reg)
        return base if is_long else f"(uint32_t)(int32_t)(int16_t){base}"

    def ea_addr(self, e, out, tag):
        """The address an EA names, for `lea`, `pea`, `jmp` and `jsr`."""
        op = self.ea(e, 4, out, tag)
        if op is None or op.kind != "mem":
            return None
        return op.expr

    # ------------------------------------------------------------------
    def push(self, size, value):
        return [f"    {A(7)} -= {size};", f"    M68K_W{BITS[size]}({A(7)}, {value});"]

    def pop(self, size, into):
        return [f"    uint32_t {into} = M68K_R{BITS[size]}({A(7)}); {A(7)} += {size};"]

    # ------------------------------------------------------------------
    def insn(self, ins):
        """C statements for one instruction that is not a control transfer."""
        out = []
        m = ins.mnem
        sz = ins.opsize
        # Exactly one trailing size letter: `rstrip` would take `subw` to
        # `su` and `asll` to `as`, since their stems end in one too.
        base = m[:-1] if m[-1] in "bwl" else m
        if sz is None and m[-1] in "bwl" and base in (
                "move", "movea", "add", "sub", "and", "or", "eor", "cmp",
                "cmpa", "adda", "suba", "addi", "subi", "andi", "ori", "eori",
                "cmpi", "addq", "subq", "clr", "neg", "negx", "not", "tst",
                "asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr",
                "addx", "subx", "ext", "movem", "link", "muls", "mulu",
                "divs", "divu", "chk", "movep"):
            sz = {"b": 1, "w": 2, "l": 4}[m[-1]]

        w = ins.word
        rx = (w >> 9) & 7
        eas = ins.eas

        def two():
            """Source and destination operands, in decode order."""
            if len(eas) != 2:
                return None, None
            return (self.ea(eas[0], sz, out, "s"),
                    self.ea(eas[1], sz, out, "d"))

        # -- move / movea ----------------------------------------------
        if base == "move" and len(eas) == 2:
            s, d = two()
            if s is None or d is None:
                self.unhandled(ins, out); return out
            out.append(f"    uint32_t v = {s.read()};")
            out.append(f"    {d.write('v')}")
            out.append(f"    m68k_f_logic(c, v, {sz});")
            return out
        if base == "movea":
            s = self.ea(eas[0], sz, out, "s")
            if s is None:
                self.unhandled(ins, out); return out
            out.append(f"    {A(rx)} = (uint32_t){SCAST[sz]}{s.read()};")
            return out
        if m == "moveq":
            out.append(f"    uint32_t v = 0x{(w & 0xFF) - (0x100 if w & 0x80 else 0) & 0xFFFFFFFF:08X}u;")
            out.append(f"    {D(rx)} = v;")
            out.append("    m68k_f_logic(c, v, 4);")
            return out

        # -- arithmetic and logic --------------------------------------
        ARITH = {"add": "+", "sub": "-", "and": "&", "or": "|", "eor": "^"}
        if base in ARITH and len(eas) == 2:
            s, d = two()
            if s is None or d is None:
                self.unhandled(ins, out); return out
            op = ARITH[base]
            out.append(f"    uint32_t s = {s.read()}, d = {d.read()};")
            out.append(f"    uint32_t r = d {op} s;")
            out.append(f"    {d.write('r')}")
            if base in ("add", "sub"):
                out.append(f"    m68k_f_{base}(c, s, d, r, {sz});")
            else:
                out.append(f"    m68k_f_logic(c, r, {sz});")
            return out
        if base == "cmp" and len(eas) == 2:
            s, d = two()
            if s is None or d is None:
                self.unhandled(ins, out); return out
            out.append(f"    uint32_t s = {s.read()}, d = {d.read()};")
            out.append(f"    m68k_f_cmp(c, s, d, d - s, {sz});")
            return out
        if base in ("adda", "suba", "cmpa"):
            s = self.ea(eas[0], sz, out, "s")
            if s is None:
                self.unhandled(ins, out); return out
            out.append(f"    uint32_t s = (uint32_t){SCAST[sz]}{s.read()};")
            if base == "adda":
                out.append(f"    {A(rx)} += s;")
            elif base == "suba":
                out.append(f"    {A(rx)} -= s;")
            else:
                out.append(f"    m68k_f_cmp(c, s, {A(rx)}, {A(rx)} - s, 4);")
            return out

        # -- immediate forms -------------------------------------------
        IMM = {"addi": "add", "subi": "sub", "andi": "and", "ori": "or",
               "eori": "eor", "cmpi": "cmp"}
        if base in IMM and ins.imm is not None and len(eas) == 1:
            d = self.ea(eas[0], sz, out, "d")
            if d is None:
                self.unhandled(ins, out); return out
            kind = IMM[base]
            out.append(f"    uint32_t s = 0x{ins.imm & 0xFFFFFFFF:08X}u, d = {d.read()};")
            if kind == "cmp":
                out.append(f"    m68k_f_cmp(c, s, d, d - s, {sz});")
                return out
            op = {"add": "+", "sub": "-", "and": "&", "or": "|", "eor": "^"}[kind]
            out.append(f"    uint32_t r = d {op} s;")
            out.append(f"    {d.write('r')}")
            if kind in ("add", "sub"):
                out.append(f"    m68k_f_{kind}(c, s, d, r, {sz});")
            else:
                out.append(f"    m68k_f_logic(c, r, {sz});")
            return out
        if base in ("addq", "subq") and len(eas) == 1:
            n = rx if rx else 8
            d = self.ea(eas[0], sz, out, "d")
            if d is None:
                self.unhandled(ins, out); return out
            # An address register is a full 32-bit add whatever the size, and
            # the condition codes are left alone. That is the one case where the
            # size suffix does not describe what happens.
            if d.kind == "a":
                out.append(f"    {d.expr} {'+' if base == 'addq' else '-'}= {n};")
                return out
            out.append(f"    uint32_t s = {n}u, d = {d.read()};")
            out.append(f"    uint32_t r = d {'+' if base == 'addq' else '-'} s;")
            out.append(f"    {d.write('r')}")
            out.append(f"    m68k_f_{'add' if base == 'addq' else 'sub'}"
                       f"(c, s, d, r, {sz});")
            return out

        # -- extended precision ----------------------------------------
        if base in ("addx", "subx") and len(eas) == 2:
            s, d = two()
            if s is None or d is None:
                self.unhandled(ins, out); return out
            out.append(f"    uint32_t s = {s.read()}, d = {d.read()};")
            sign = "+" if base == "addx" else "-"
            out.append(f"    uint32_t r = d {sign} s {sign} c->x;")
            out.append(f"    {d.write('r')}")
            out.append(f"    m68k_f_{base}(c, s, d, r, {sz});")
            return out

        # -- single operand --------------------------------------------
        if base in ("clr", "neg", "negx", "not", "tst") and len(eas) == 1:
            d = self.ea(eas[0], sz, out, "d")
            if d is None:
                self.unhandled(ins, out); return out
            if base == "clr":
                out.append(f"    {d.write('0u')}")
                out.append(f"    m68k_f_logic(c, 0u, {sz});")
            elif base == "tst":
                out.append(f"    m68k_f_logic(c, {d.read()}, {sz});")
            elif base == "not":
                out.append(f"    uint32_t r = ~{d.read()};")
                out.append(f"    {d.write('r')}")
                out.append(f"    m68k_f_logic(c, r, {sz});")
            else:
                borrow = " - c->x" if base == "negx" else ""
                out.append(f"    uint32_t d = {d.read()};")
                out.append(f"    uint32_t r = 0u - d{borrow};")
                out.append(f"    {d.write('r')}")
                out.append(f"    m68k_f_{'subx' if base == 'negx' else 'sub'}"
                           f"(c, d, 0u, r, {sz});")
            return out
        if base == "ext":
            src = 1 if sz == 2 else 2
            out.append(f"    uint32_t r = (uint32_t){SCAST[src]}{D(w & 7)};")
            out.append(f"    {Operand('d', D(w & 7), sz).write('r')}")
            out.append(f"    m68k_f_logic(c, r, {sz});")
            return out
        if m == "swap":
            out.append(f"    uint32_t r = ({D(w & 7)} >> 16) | ({D(w & 7)} << 16);")
            out.append(f"    {D(w & 7)} = r;")
            out.append("    m68k_f_logic(c, r, 4);")
            return out
        if m == "tas":
            d = self.ea(eas[0], 1, out, "d")
            if d is None:
                self.unhandled(ins, out); return out
            out.append(f"    uint32_t d = {d.read()};")
            out.append("    m68k_f_logic(c, d, 1);")
            out.append(f"    {d.write('d | 0x80u')}")
            return out

        # -- bit operations --------------------------------------------
        if m in ("btst", "bset", "bclr", "bchg"):
            # Static form takes the bit number from an extension word, dynamic
            # from a data register. A register destination is 32 bits wide and
            # anything in memory is a byte, which sets the modulus.
            dyn = (w & 0x0100) != 0
            d = self.ea(eas[-1], 1, out, "d")
            if d is None:
                self.unhandled(ins, out); return out
            width = 32 if d.kind == "d" else 8
            if d.kind == "d":
                d = Operand("d", d.expr, 4)
            src = f"({D(rx)} & {width - 1}u)" if dyn else f"{ins.imm % width}u"
            out.append(f"    uint32_t bit = 1u << {src};")
            out.append(f"    uint32_t d = {d.read()};")
            out.append("    c->z = (d & bit) == 0;")
            if m != "btst":
                expr = {"bset": "d | bit", "bclr": "d & ~bit", "bchg": "d ^ bit"}[m]
                out.append(f"    {d.write(expr)}")
            return out

        # -- shifts and rotates ----------------------------------------
        if base in ("asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr"):
            return self.shift(ins, base, sz, out)

        # -- multiply and divide ---------------------------------------
        if base in ("muls", "mulu"):
            s = self.ea(eas[0], 2, out, "s")
            if s is None:
                self.unhandled(ins, out); return out
            if base == "muls":
                out.append(f"    uint32_t r = (uint32_t)((int32_t)(int16_t){s.read()}"
                           f" * (int32_t)(int16_t){D(rx)});")
            else:
                out.append(f"    uint32_t r = (uint32_t)((uint16_t){s.read()}"
                           f" * (uint16_t){D(rx)});")
            out.append(f"    {D(rx)} = r;")
            out.append("    m68k_f_logic(c, r, 4);")
            return out
        if base in ("divs", "divu"):
            s = self.ea(eas[0], 2, out, "s")
            if s is None:
                self.unhandled(ins, out); return out
            out.append(f"    uint32_t dv = {s.read()};")
            out.append("    if (dv == 0) { c->v = 0; c->c = 0; }")
            out.append("    else {")
            if base == "divs":
                out.append(f"        int32_t q = (int32_t){D(rx)} / (int32_t)(int16_t)dv;")
                out.append(f"        int32_t rem = (int32_t){D(rx)} % (int32_t)(int16_t)dv;")
                out.append("        if (q > 32767 || q < -32768) { c->v = 1; c->c = 0; }")
            else:
                out.append(f"        uint32_t q = {D(rx)} / (uint16_t)dv;")
                out.append(f"        uint32_t rem = {D(rx)} % (uint16_t)dv;")
                out.append("        if (q > 0xFFFFu) { c->v = 1; c->c = 0; }")
            out.append("        else {")
            out.append(f"            {D(rx)} = ((uint32_t)rem << 16) | (uint16_t)q;")
            out.append("            m68k_f_logic(c, (uint32_t)(uint16_t)q, 2);")
            out.append("        }")
            out.append("    }")
            return out

        # -- addresses and frames --------------------------------------
        if m == "lea":
            a = self.ea_addr(eas[0], out, "s")
            if a is None:
                self.unhandled(ins, out); return out
            out.append(f"    {A(rx)} = {a};")
            return out
        if m == "pea":
            a = self.ea_addr(eas[0], out, "s")
            if a is None:
                self.unhandled(ins, out); return out
            out.append(f"    uint32_t p = {a};")
            out += self.push(4, "p")
            return out
        if base == "link":
            r = w & 7
            out += self.push(4, A(r))
            out.append(f"    {A(r)} = {A(7)};")
            out.append(f"    {A(7)} += {ins.imm};")
            return out
        if m == "unlk":
            r = w & 7
            out.append(f"    {A(7)} = {A(r)};")
            out += self.pop(4, "fp")
            out.append(f"    {A(r)} = fp;")
            return out
        if m == "exg":
            # The register pair is encoded in the opmode, not the operand text.
            opm = (w >> 3) & 0x1F
            other = w & 7
            if opm == 0x08:
                x, y = D(rx), D(other)
            elif opm == 0x09:
                x, y = A(rx), A(other)
            else:
                x, y = D(rx), A(other)
            out.append(f"    {{ uint32_t t = {x}; {x} = {y}; {y} = t; }}")
            return out

        # -- movem -----------------------------------------------------
        if base == "movem":
            return self.movem(ins, sz, out)

        # -- status register -------------------------------------------
        if m == "movew" and ins.ops.endswith(",%sr"):
            s = self.ea(eas[0], 2, out, "s")
            out.append(f"    m68k_set_sr(c, (uint16_t){s.read()});")
            return out
        if m == "movew" and ins.ops.startswith("%sr,"):
            d = self.ea(eas[0], 2, out, "d")
            out.append(f"    {d.write('m68k_get_sr(c)')}")
            return out
        if m == "movew" and ins.ops.endswith(",%ccr"):
            s = self.ea(eas[0], 2, out, "s")
            out.append(f"    m68k_set_ccr(c, (uint8_t){s.read()});")
            return out
        if m == "movel" and ins.ops.endswith(",%usp"):
            out.append(f"    c->usp = {A(w & 7)};")
            return out
        if m == "movel" and ins.ops.startswith("%usp,"):
            out.append(f"    {A(w & 7)} = c->usp;")
            return out
        if m in ("orib", "andib", "eorib") and ins.ops.endswith(",%ccr"):
            op = {"orib": "|", "andib": "&", "eorib": "^"}[m]
            out.append(f"    m68k_set_ccr(c, (uint8_t)(m68k_get_sr(c) {op} "
                       f"0x{(w >> 0) & 0xFF:02X}u));")
            return out
        if m in ("oriw", "andiw", "eoriw") and ins.ops.endswith(",%sr"):
            op = {"oriw": "|", "andiw": "&", "eoriw": "^"}[m]
            out.append(f"    m68k_set_sr(c, (uint16_t)(m68k_get_sr(c) {op} "
                       f"0x{ins.imm & 0xFFFF:04X}u));")
            return out

        # -- conditional set -------------------------------------------
        if m.startswith("s") and m[1:] in CC and len(eas) == 1:
            d = self.ea(eas[0], 1, out, "d")
            if d is None:
                self.unhandled(ins, out); return out
            out.append(f"    {d.write(f'({CC[m[1:]]}) ? 0xFFu : 0x00u')}")
            return out

        if m == "nop":
            return ["    /* nop */"]
        if m == "reset":
            return ["    /* reset: no external bus to pulse */"]

        self.unhandled(ins, out)
        return out

    # ------------------------------------------------------------------
    def shift(self, ins, base, sz, out):
        """Shifts and rotates.

        Memory forms shift one bit of a word in place; register forms take the
        count from an immediate 1-8 or, modulo 64, from a data register. The
        flags are what separate the eight: X is left alone by the plain rotates,
        V is only ever set by `asl`, and a zero count clears C but leaves X.
        """
        w = ins.word
        if (w & 0xC0) == 0xC0:                    # memory form, always one bit
            d = self.ea(ins.eas[0], 2, out, "d")
            if d is None:
                self.unhandled(ins, out); return out
            cnt, sz = "1u", 2
        else:
            d = Operand("d", D(w & 7), sz)
            if w & 0x20:
                cnt = f"({D((w >> 9) & 7)} & 63u)"
            else:
                cnt = f"{((w >> 9) & 7) or 8}u"
        # Two cycles a bit, and only the immediate form's count is known at
        # build time — `cost()` adds that one. This is the other form, charged
        # where the count exists.
        by_reg = (w & 0xC0) != 0xC0 and (w & 0x20)
        left = base in ("asl", "lsl", "rol", "roxl")
        bits = BITS[sz]
        out.append(f"    uint32_t n = {cnt}, d = {d.read()}, r = d;")
        if by_reg:
            out.append("    m68k_fuel -= (int32_t)(n << 1);")
        out.append(f"    uint32_t last = c->c;")
        out.append("    if (n == 0) { c->c = 0; }")
        out.append("    else {")
        out.append(f"        for (uint32_t i = 0; i < n; i++) {{")
        if base in ("asl", "lsl"):
            out.append(f"            last = (r >> {bits - 1}) & 1u;")
            out.append("            r <<= 1;")
        elif base == "asr":
            out.append("            last = r & 1u;")
            out.append(f"            r = (r >> 1) | (r & (1u << {bits - 1}));")
        elif base == "lsr":
            out.append("            last = r & 1u;")
            out.append("            r >>= 1;")
        elif base == "rol":
            out.append(f"            last = (r >> {bits - 1}) & 1u;")
            out.append(f"            r = (r << 1) | last;")
        elif base == "ror":
            out.append("            last = r & 1u;")
            out.append(f"            r = (r >> 1) | (last << {bits - 1});")
        elif base == "roxl":
            out.append(f"            uint32_t hi = (r >> {bits - 1}) & 1u;")
            out.append("            r = (r << 1) | c->x;")
            out.append("            c->x = hi; last = hi;")
        else:                                     # roxr
            out.append("            uint32_t lo = r & 1u;")
            out.append(f"            r = (r >> 1) | (c->x << {bits - 1});")
            out.append("            c->x = lo; last = lo;")
        out.append(f"            r &= M68K_MASK({sz});")
        out.append("        }")
        out.append("        c->c = last;")
        if base not in ("rol", "ror", "roxl", "roxr"):
            out.append("        c->x = last;")
        out.append("    }")
        out.append(f"    {d.write('r')}")
        out.append(f"    c->n = (r & M68K_MSB({sz})) != 0;")
        out.append(f"    c->z = (r & M68K_MASK({sz})) == 0;")
        if base == "asl":
            # V is set when the sign changed at any point during the shift,
            # which is the one flag rule here that a single-step model misses.
            out.append(f"    {{ uint32_t keep = n >= {bits} ? M68K_MASK({sz}) : "
                       f"(M68K_MASK({sz}) << ({bits} - 1 - (n >= {bits} ? {bits} - 1 : n)))"
                       f" & M68K_MASK({sz});")
            out.append(f"      uint32_t top = d & keep;")
            out.append(f"      c->v = !(top == 0 || top == keep); }}")
        else:
            out.append("    c->v = 0;")
        return out

    # ------------------------------------------------------------------
    def movem(self, ins, sz, out):
        """Register list to or from memory.

        The mask's bit 0 is D0 and bit 15 is A7 — except for `-(An)`, where the
        order reverses, so bit 0 is A7. That is not a quirk of the encoding but
        of the direction: predecrement walks down, and reversing the list is
        what makes the registers land in the same order either way.

        A word transfer *into* a register sign-extends into all 32 bits, which
        is the one thing about `movem.w` that catches people out.
        """
        w, mask = ins.word, ins.imm
        to_mem = (w & 0x0400) == 0
        e = ins.eas[0]
        regs = [D(i) for i in range(8)] + [A(i) for i in range(8)]
        order = list(reversed(regs)) if e.mode == 4 else regs
        picked = [(b, order[b]) for b in range(16) if mask & (1 << b)]

        if e.mode == 4:
            out.append(f"    uint32_t p = {A(e.reg)};")
            for _, r in picked:
                out.append(f"    p -= {sz}; M68K_W{BITS[sz]}(p, {UCAST[sz]}{r});")
            out.append(f"    {A(e.reg)} = p;")
            return out

        addr = self.ea_addr(e, out, "m")
        if addr is None:
            self.unhandled(ins, out)
            return out
        out.append(f"    uint32_t p = {addr};")
        for _, r in picked:
            if to_mem:
                out.append(f"    M68K_W{BITS[sz]}(p, {UCAST[sz]}{r}); p += {sz};")
            else:
                out.append(f"    {r} = (uint32_t){SCAST[sz]}M68K_R{BITS[sz]}(p);"
                           f" p += {sz};")
        if e.mode == 3:                            # (An)+ writes the pointer back
            out.append(f"    {A(e.reg)} = p;")
        return out

    # ------------------------------------------------------------------
    def transfer(self, ins, fallthrough, own=frozenset()):
        """C statements for a control transfer, ending the block.

        A destination this function owns becomes a `goto`; anything else is
        returned to the trampoline, which is how a tail call into a shared
        block, or into a function discovered separately, stays correct without
        the two having to know about each other.
        """
        out = []
        m, k = ins.mnem, ins.kind
        after = ins.addr + ins.size

        def to(addr):
            return (f"    goto {lname(addr)};" if addr in own
                    else f"    return 0x{addr:08X}u;")

        if k == RET:
            if m == "rts":
                out += self.pop(4, "ret")
                out.append("    return ret;")
            elif m == "rte":
                out += self.pop(2, "sr")
                out += self.pop(4, "ret")
                out.append("    m68k_set_sr(c, (uint16_t)sr);")
                out.append("    return ret;")
            elif m == "rtr":
                out += self.pop(2, "ccr")
                out += self.pop(4, "ret")
                out.append("    m68k_set_ccr(c, (uint8_t)ccr);")
                out.append("    return ret;")
            else:                                  # stop
                out.append(f"    return 0x{after:08X}u;   /* stop */")
            return out

        if k == TRAP:
            self.notes.append((ins.addr, ins.text()))
            out.append(f"    /* {ins.text()} -- no exception model */")
            out.append(f"    return 0x{after:08X}u;")
            return out

        if k == CALL:                              # bsr / jsr to a known target
            out += self.push(4, f"0x{after:08X}u")
            out.append(f"    return 0x{ins.target:08X}u;")
            return out
        if k == CALL_IND:
            a = self.ea_addr(ins.eas[0], out, "t")
            if a is None:
                self.unhandled(ins, out)
                out.append(f"    return 0x{after:08X}u;")
                return out
            out.append(f"    uint32_t t = {a};")
            out += self.push(4, f"0x{after:08X}u")
            out.append("    return t;")
            return out
        if k == JUMP_IND:
            a = self.ea_addr(ins.eas[0], out, "t")
            if a is None:
                self.unhandled(ins, out)
                out.append("    return 0u;")
                return out
            out.append(f"    return {a};")
            return out
        if k == JUMP:                              # bra / jmp to a known target
            out.append(to(ins.target) if ins.target is not None
                       else f"    return 0x{after:08X}u;")
            return out

        if k == BRANCH:
            # The block already paid this instruction's table cost, which for a
            # branch is the *taken* one; each edge that costs something else
            # settles the difference here. Adding fuel back is a discount.
            if m.startswith("db"):
                cc = m[2:]
                # `dbcc` falls through when the condition holds; otherwise it
                # decrements the low word and loops unless it wrapped past zero.
                # `dbf` is the common one and its condition is never true, so
                # this is the ordinary counted loop.
                #
                # Three edges and three prices: 12 charged, 10 when it loops
                # (CYC_DBCC_F_NOEXP), 14 when the counter expired
                # (CYC_DBCC_F_EXP), and 12 unchanged when the condition was true
                # and it never counted at all.
                r = D(ins.word & 7)
                out.append(f"    if (!({CC[cc]})) {{")
                out.append(f"        uint32_t lo = ({r} - 1) & 0xFFFFu;")
                out.append(f"        {r} = ({r} & 0xFFFF0000u) | lo;")
                out.append(f"        if (lo != 0xFFFFu) {{ m68k_fuel += 2; "
                           f"{to(ins.target).strip()} }}")
                out.append("        m68k_fuel -= 2;")
                out.append("    }")
            else:
                # `bccs` is bcc with a short displacement, not b + "ccs" — and
                # stripping trailing s/w wholesale turns it into `bc`.
                cc = m[1:]
                if cc not in CC:
                    cc = cc[:-1]
                out.append(f"    if ({CC[cc]}) {to(ins.target).strip()}")
                # Not taken: a short branch is two cycles cheaper than the taken
                # one and a word branch two dearer, which is the whole of
                # CYC_BCC_NOTAKE_B and _W.
                out.append("    m68k_fuel += 2;" if ins.size == 2
                           else "    m68k_fuel -= 2;")
            out.append(to(fallthrough) if fallthrough is not None
                       else f"    return 0x{after:08X}u;")
            return out

        return out

    # ------------------------------------------------------------------
    def function(self, fn):
        """One C function: an entry dispatch, then a label per basic block.

        Every block ends in an explicit `goto` or `return` — the SH-2 side
        learned that the hard way, where falling out of a block was textual
        adjacency and 288 functions own two disjoint runs. A block whose
        successor belongs to someone else hands the address back and lets the
        trampoline find its owner, which is what makes a shared tail work.
        """
        TRANSFERS = (BRANCH, JUMP, JUMP_IND, CALL, CALL_IND, RET, TRAP)
        blocks = sorted(fn.blocks.values(), key=lambda b: b.start)
        own = {b.start for b in blocks}
        lines = [f"uint32_t {fname(fn.start)}(M68K *c, uint32_t entry) {{",
                 "    switch (entry) {"]
        for b in blocks:
            lines.append(f"    case 0x{b.start:08X}u: goto {lname(b.start)};")
        lines += ["    default: return entry;", "    }"]

        for b in blocks:
            # The same yield the SH-2 blocks carry, and needed for the same
            # reason: a poll loop inside one function is a `goto` here and never
            # reaches the trampoline, so `m68k_run`'s budget can never stop it.
            # It only ever terminated because the SH-2 answered inside the
            # 68000's own register write; once the two CPUs really interleave,
            # 0x881868 waits for the master's 0xFFFF and spins for good.
            lines.append(f"{lname(b.start)}: M68K_BLOCK(c, 0x{b.start:08X}u, "
                         f"{len(b.insns)}, {self.block_cycles(b)});")
            for ins in b.insns:
                if ins.kind == INVALID:
                    self.notes.append((ins.addr, "invalid opcode"))
                    lines.append(f"    /* .short 0x{ins.word:04X} */")
                    lines.append(f"    return 0x{ins.addr:08X}u;")
                    break
                if ins.kind in TRANSFERS:
                    nxt = ins.addr + ins.size
                    lines += ["    {"]
                    lines += self.transfer(ins, nxt if nxt in own else None, own)
                    lines += ["    }"]
                    continue
                lines += ["    {"] + self.insn(ins) + ["    }"]
            if not b.insns or b.insns[-1].kind not in TRANSFERS:
                lines.append(f"    goto {lname(b.end)};" if b.end in own
                             else f"    return 0x{b.end:08X}u;")
        lines.append("}")
        return lines
