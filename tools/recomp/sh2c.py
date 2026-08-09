"""Translate discovered SH-2 functions into C.

One C function per discovered SH-2 function, basic blocks as labels, registers
in a context struct. Two things drive the shape of the output:

**Delay slots.** A delayed branch executes the instruction *after* it before
transferring, but the branch's own inputs — the condition, or the target
register — are sampled *before* that instruction runs. So a transfer is emitted
as: capture inputs into temporaries, run the delay slot, then transfer. Getting
this backwards is invisible until a delay slot happens to clobber the register
its branch jumps through.

**Nothing nests.** The SH-2 has no call stack — `bsr`/`jsr` only write PR — so
a "call" is modelled as exactly that: set PR, then transfer. `rts` transfers to
PR. Every control transfer returns its destination to the runtime trampoline,
which loops. There is no C recursion anywhere, so a handler that branches away
instead of returning (ordinary SH-2 practice, and what the slave's idle handler
does) costs nothing here either.

The consequence is that a return address must be dispatchable, so the
instruction after a call starts a block, and a function can be entered at any
of its blocks via the `entry` parameter. Code that *computes* a return address
now works too, since PR is just a context field.
"""

from sh2.decode import (BRANCH, JUMP, JUMP_IND, CALL, CALL_IND, RET, INVALID)


def fname(addr):
    return f"f_{addr:08X}"


def lname(addr):
    return f"L_{addr:08X}"


def R(n):
    return f"c->r[{n}]"


S32 = "(int32_t)"
U32 = "(uint32_t)"


class Codegen:
    def __init__(self, az, img):
        self.az = az
        self.img = img
        self.notes = []          # (addr, reason) for things outside the model
        self.escapes = 0         # branches leaving the owning function (tail transfers)

    # ------------------------------------------------------------------
    def value_at(self, addr, size):
        if not self.img.readable(addr, size):
            return None
        return self.img.u32(addr) if size == 4 else self.img.u16(addr)

    def insn(self, ins):
        """C statements for one non-control-flow instruction."""
        m, n, rm = ins.mnem, ins.rn, ins.rm
        i = ins.imm
        out = []

        def st(s):
            out.append(s)

        # -- loads and stores ------------------------------------------
        # Dispatch on the encoding rather than the rendered text: the operand
        # string is for humans, and several distinct forms print similarly.
        if m in ("mov.b", "mov.w", "mov.l"):
            sz = {"mov.b": 1, "mov.w": 2, "mov.l": 4}[m]
            rd = {1: "sh2_r8", 2: "sh2_r16", 4: "sh2_r32"}[sz]
            wr = {1: "sh2_w8", 2: "sh2_w16", 4: "sh2_w32"}[sz]
            sext = {1: "(int32_t)(int8_t)", 2: "(int32_t)(int16_t)", 4: "(int32_t)"}[sz]
            cast = {1: "(uint8_t)", 2: "(uint16_t)", 4: "(uint32_t)"}[sz]
            w, hi, lo4 = ins.word, (ins.word >> 12) & 0xF, ins.word & 0xF

            def load(addr_expr, dst=None):
                st(f"{R(dst if dst is not None else n)} = "
                   f"{U32}{sext}{rd}(c, {addr_expr});")

            if ins.pool:                                   # mov.w/l @(d,pc),Rn
                load(f"0x{ins.pool[0]:08X}u")
            elif hi == 0x0 and lo4 in (0x4, 0x5, 0x6):     # Rm,@(r0,Rn)
                st(f"{wr}(c, {R(n)} + {R(0)}, {cast}{R(rm)});")
            elif hi == 0x0 and lo4 in (0xC, 0xD, 0xE):     # @(r0,Rm),Rn
                load(f"{R(rm)} + {R(0)}")
            elif hi == 0x1:                                # mov.l Rm,@(d,Rn)
                st(f"{wr}(c, {R(n)} + {i}, {cast}{R(rm)});")
            elif hi == 0x5:                                # mov.l @(d,Rm),Rn
                load(f"{R(rm)} + {i}")
            elif hi == 0x2 and lo4 in (0x0, 0x1, 0x2):     # Rm,@Rn
                st(f"{wr}(c, {R(n)}, {cast}{R(rm)});")
            elif hi == 0x2 and lo4 in (0x4, 0x5, 0x6):     # Rm,@-Rn
                st(f"{R(n)} -= {sz};")
                st(f"{wr}(c, {R(n)}, {cast}{R(rm)});")
            elif hi == 0x6 and lo4 in (0x0, 0x1, 0x2):     # @Rm,Rn
                load(f"{R(rm)}")
            elif hi == 0x6 and lo4 in (0x4, 0x5, 0x6):     # @Rm+,Rn
                # Rm == Rn is legal; the load must happen before the bump.
                st(f"{{ uint32_t _a = {R(rm)}; {R(rm)} += {sz};"
                   f" {R(n)} = {U32}{sext}{rd}(c, _a); }}")
            elif hi == 0x8 and ((w >> 8) & 0xF) in (0x0, 0x1):   # R0,@(d,Rn)
                st(f"{wr}(c, {R(n)} + {i}, {cast}{R(0)});")
            elif hi == 0x8 and ((w >> 8) & 0xF) in (0x4, 0x5):   # @(d,Rm),R0
                load(f"{R(rm)} + {i}", dst=0)
            elif hi == 0xC and ((w >> 8) & 0xF) in (0x0, 0x1, 0x2):   # R0,@(d,gbr)
                st(f"{wr}(c, c->gbr + {i * sz}, {cast}{R(0)});")
            elif hi == 0xC and ((w >> 8) & 0xF) in (0x4, 0x5, 0x6):   # @(d,gbr),R0
                load(f"c->gbr + {i * sz}", dst=0)
            else:
                st(f"/* UNHANDLED {m} {ins.ops} */")
                self.notes.append((ins.addr, f"{m} {ins.ops}"))
            return out

        if m == "mov":
            st(f"{R(n)} = {U32}{i};" if i is not None else f"{R(n)} = {R(rm)};")
            return out
        if m == "mova":
            st(f"{R(0)} = 0x{ins.imm:08X}u;")
            return out

        # Immediate logic on R0 shares mnemonics with the register forms, so it
        # has to be settled before the register table below.
        if m in ("and", "or", "xor") and rm is None:
            op = {"and": "&=", "or": "|=", "xor": "^="}[m]
            st(f"{R(0)} {op} {i}u;")
            return out

        # -- arithmetic / logic ----------------------------------------
        simple = {
            "add": f"{R(n)} += {R(rm)};" if i is None else f"{R(n)} += {U32}{i};",
            "sub": f"{R(n)} -= {R(rm)};",
            "and": f"{R(n)} &= {R(rm)};",
            "or": f"{R(n)} |= {R(rm)};",
            "xor": f"{R(n)} ^= {R(rm)};",
            "not": f"{R(n)} = ~{R(rm)};",
            "neg": f"{R(n)} = 0u - {R(rm)};",
            "swap.b": f"{R(n)} = sh2_swapb({R(rm)});",
            "swap.w": f"{R(n)} = sh2_swapw({R(rm)});",
            "xtrct": f"{R(n)} = sh2_xtrct({R(rm)}, {R(n)});",
            "exts.b": f"{R(n)} = {U32}(int32_t)(int8_t){R(rm)};",
            "exts.w": f"{R(n)} = {U32}(int32_t)(int16_t){R(rm)};",
            "extu.b": f"{R(n)} = {R(rm)} & 0xFFu;",
            "extu.w": f"{R(n)} = {R(rm)} & 0xFFFFu;",
            "dt": f"{R(n)} -= 1; c->t = ({R(n)} == 0);",
            "movt": f"{R(n)} = c->t;",
            "clrt": "c->t = 0;",
            "sett": "c->t = 1;",
            "clrmac": "c->mach = c->macl = 0;",
            "nop": "",
            "sleep": "/* sleep */",
        }
        if m in simple:
            if simple[m]:
                st(simple[m])
            return out

        if m in ("addc", "subc", "addv", "subv", "negc"):
            st(f"sh2_{m.replace('.', '')}(c, {n}, {rm});")
            return out
        if m in ("div0s",):
            st(f"sh2_div0s(c, {n}, {rm});")
            return out
        if m == "div0u":
            st("sh2_div0u(c);")
            return out
        if m == "div1":
            st(f"sh2_div1(c, {n}, {rm});")
            return out
        if m in ("dmuls.l", "dmulu.l"):
            st(f"sh2_{'dmuls' if m.startswith('dmuls') else 'dmulu'}(c, {n}, {rm});")
            return out
        if m == "mul.l":
            st(f"c->macl = {R(n)} * {R(rm)};")
            return out
        if m == "muls.w":
            st(f"c->macl = {U32}((int32_t)(int16_t){R(n)} * (int32_t)(int16_t){R(rm)});")
            return out
        if m == "mulu.w":
            st(f"c->macl = ({R(n)} & 0xFFFFu) * ({R(rm)} & 0xFFFFu);")
            return out
        if m == "mac.w":
            st(f"sh2_macw(c, {n}, {rm});")
            return out
        if m == "mac.l":
            st(f"sh2_macl(c, {n}, {rm});")
            return out

        # -- comparisons ------------------------------------------------
        cmps = {
            "cmp/eq": f"c->t = ({R(n)} == {R(rm)});",
            "cmp/hs": f"c->t = ({R(n)} >= {R(rm)});",
            "cmp/ge": f"c->t = ({S32}{R(n)} >= {S32}{R(rm)});",
            "cmp/hi": f"c->t = ({R(n)} > {R(rm)});",
            "cmp/gt": f"c->t = ({S32}{R(n)} > {S32}{R(rm)});",
            "cmp/pl": f"c->t = ({S32}{R(n)} > 0);",
            "cmp/pz": f"c->t = ({S32}{R(n)} >= 0);",
            "cmp/str": f"c->t = sh2_cmpstr({R(n)}, {R(rm)});",
        }
        if m in cmps:
            if m == "cmp/eq" and i is not None:
                st(f"c->t = ({S32}{R(0)} == {i});")
            else:
                st(cmps[m])
            return out
        if m == "tst":
            if i is not None:
                st(f"c->t = (({R(0)} & {i}u) == 0);")
            else:
                st(f"c->t = (({R(n)} & {R(rm)}) == 0);")
            return out
        if m == "tst.b":
            st(f"c->t = ((sh2_r8(c, c->gbr + {R(0)}) & {i}u) == 0);")
            return out
        if m in ("and.b", "or.b", "xor.b"):
            op = {"and.b": "&", "or.b": "|", "xor.b": "^"}[m]
            st(f"{{ uint32_t a = c->gbr + {R(0)};"
               f" sh2_w8(c, a, (uint8_t)(sh2_r8(c, a) {op} {i}u)); }}")
            return out
        # -- shifts and rotates ----------------------------------------
        shifts = {
            "shll": f"c->t = {R(n)} >> 31; {R(n)} <<= 1;",
            "shal": f"c->t = {R(n)} >> 31; {R(n)} <<= 1;",
            "shlr": f"c->t = {R(n)} & 1; {R(n)} >>= 1;",
            "shar": f"c->t = {R(n)} & 1; {R(n)} = {U32}({S32}{R(n)} >> 1);",
            "shll2": f"{R(n)} <<= 2;", "shlr2": f"{R(n)} >>= 2;",
            "shll8": f"{R(n)} <<= 8;", "shlr8": f"{R(n)} >>= 8;",
            "shll16": f"{R(n)} <<= 16;", "shlr16": f"{R(n)} >>= 16;",
            "rotl": f"c->t = {R(n)} >> 31; {R(n)} = ({R(n)} << 1) | c->t;",
            "rotr": f"c->t = {R(n)} & 1; {R(n)} = ({R(n)} >> 1) | (c->t << 31);",
            "rotcl": f"{{ uint32_t b = {R(n)} >> 31;"
                     f" {R(n)} = ({R(n)} << 1) | c->t; c->t = b; }}",
            "rotcr": f"{{ uint32_t b = {R(n)} & 1;"
                     f" {R(n)} = ({R(n)} >> 1) | (c->t << 31); c->t = b; }}",
        }
        if m in shifts:
            st(shifts[m])
            return out

        # -- system registers -------------------------------------------
        sysmap = {"mach": "c->mach", "macl": "c->macl", "pr": "c->pr",
                  "sr": None, "gbr": "c->gbr", "vbr": "c->vbr"}
        if m in ("sts", "stc"):
            src = ins.ops.split(",")[0]
            if src == "sr":
                st(f"{R(n)} = sh2_get_sr(c);")
            else:
                st(f"{R(n)} = {sysmap[src]};")
            return out
        if m in ("sts.l", "stc.l"):
            src = ins.ops.split(",")[0]
            val = "sh2_get_sr(c)" if src == "sr" else sysmap[src]
            st(f"{R(n)} -= 4; sh2_w32(c, {R(n)}, {val});")
            return out
        if m in ("lds", "ldc"):
            dst = ins.ops.split(",")[1]
            if dst == "sr":
                st(f"sh2_set_sr(c, {R(n)});")
            else:
                st(f"{sysmap[dst]} = {R(n)};")
            return out
        if m in ("lds.l", "ldc.l"):
            dst = ins.ops.split(",")[1]
            if dst == "sr":
                st(f"sh2_set_sr(c, sh2_r32(c, {R(n)}));")
            else:
                st(f"{sysmap[dst]} = sh2_r32(c, {R(n)});")
            st(f"{R(n)} += 4;")
            return out
        if m == "tas.b":
            st(f"sh2_tas(c, {R(n)});")
            return out
        if m == "trapa":
            st(f"sh2_unimplemented(c, 0x{ins.addr:08X}u, \"trapa\");")
            return out
        if m == ".word":
            st(f"sh2_unimplemented(c, 0x{ins.addr:08X}u, \"invalid opcode\");")
            return out

        st(f"/* UNHANDLED {m} {ins.ops} */")
        self.notes.append((ins.addr, f"{m} {ins.ops}"))
        return out

    # ------------------------------------------------------------------
    def _goto(self, target, fn, indent):
        """Transfer to `target`: a local label, or out to the trampoline."""
        pad = " " * indent
        if target in fn.blocks:
            return [f"{pad}goto {lname(target)};"]
        self.escapes += 1
        return [f"{pad}return 0x{target:08X}u;"]

    def _call(self, target, ret, indent):
        """A call is a PR write plus a transfer — there is no stack frame."""
        pad = " " * indent
        return [f"{pad}c->pr = 0x{ret:08X}u;",
                f"{pad}return 0x{target:08X}u;"]

    def transfer(self, ins, ds, fn):
        """Emit a control transfer plus its delay slot, in the right order."""
        out = []
        ds_stmts = ["    " + s for s in self.insn(ds)] if ds is not None else []
        ret = ins.addr + 4          # return address is past the delay slot

        if ins.kind == BRANCH:
            cond = "c->t" if ins.mnem in ("bt", "bt.s") else "!c->t"
            if not ins.delay:
                out.append(f"    if ({cond}) {{")
                out += self._goto(ins.target, fn, 8)
                out.append("    }")
                return out
            # Condition is sampled before the delay slot runs.
            out.append("    {")
            out.append(f"        uint32_t _c = {cond};")
            out += ["    " + s for s in ds_stmts]
            out.append("        if (_c) {")
            out += self._goto(ins.target, fn, 12)
            out.append("        }")
            out.append("    }")
            return out

        if ins.kind == JUMP:
            out += ds_stmts
            out += self._goto(ins.target, fn, 4)
            return out

        if ins.kind == CALL:
            out += ds_stmts
            out += self._call(ins.target, ret, 4)
            return out

        if ins.kind in (CALL_IND, JUMP_IND):
            # The target register is read at the branch, not after the delay
            # slot, so capture it first.
            if ins.mnem in ("bsrf", "braf"):
                expr = f"0x{ins.addr + 4:08X}u + {R(ins.rn)}"
            else:
                expr = R(ins.rn)
            out.append("    {")
            out.append(f"        uint32_t _t = {expr};")
            out += ["    " + s for s in ds_stmts]
            out.append(f"        c->pr = 0x{ret:08X}u;" if ins.kind == CALL_IND
                       else "        /* jump */")
            out.append("        return _t;")
            out.append("    }")
            return out

        if ins.kind == RET:
            out += ds_stmts
            if ins.mnem == "rte":
                # rte resumes from the stacked PC, not PR; no interrupt is
                # delivered to recompiled code yet, so this ends the run.
                out.append("    return 0;")
            else:
                out.append("    return c->pr;")
            return out

        out.append(f"    /* UNHANDLED transfer {ins.mnem} */")
        self.notes.append((ins.addr, f"transfer {ins.mnem}"))
        return out

    # ------------------------------------------------------------------
    def function(self, fn):
        blocks = sorted(fn.blocks.values(), key=lambda b: b.start)
        lines = [f"/* 0x{fn.start:08X}  {fn.size()} bytes, "
                 f"{len(fn.blocks)} block(s) */",
                 f"uint32_t {fname(fn.start)}(SH2 *c, uint32_t entry) {{"]
        # Control can arrive at any block: through PR on return from a call, or
        # through a tail transfer from another function.
        if len(blocks) > 1:
            lines.append("    switch (entry) {")
            for b in blocks[1:]:
                lines.append(f"    case 0x{b.start:08X}u: goto {lname(b.start)};")
            lines.append("    default: break;")
            lines.append("    }")
        else:
            lines.append("    (void)entry;")
        for b in blocks:
            lines.append(f"{lname(b.start)}: ;")
            i = 0
            while i < len(b.insns):
                ins = b.insns[i]
                if ins.kind in (BRANCH, JUMP, JUMP_IND, CALL, CALL_IND, RET):
                    ds = b.insns[i + 1] if (ins.delay and i + 1 < len(b.insns)) else None
                    lines += self.transfer(ins, ds, fn)
                    i += 2 if ds is not None else 1
                    continue
                for s in self.insn(ins):
                    lines.append("    " + s)
                i += 1
        # A block that ends by falling off the end of the function returns.
        lines.append("    return 0;")
        lines.append("}")
        return lines
