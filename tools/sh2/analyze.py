"""Recursive-descent code discovery for SH-2.

SH-2 cannot encode a 32-bit constant inline, so compilers park constants and
code addresses in literal pools interleaved with the instruction stream and
reach them via `mov.l @(disp,pc),Rn`. Two consequences drive this module:

  1. Those pool words sit *inside* functions and must not be disassembled as
     code, so a plain linear sweep produces garbage.
  2. Almost every indirect call is `mov.l @(disp,pc),Rn` followed by `jsr @Rn`,
     so tracking literal loads is what recovers the call graph.

We therefore walk the code recursively from known entry points, propagate
constants within each basic block, and record pool words as data.
"""

import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Optional

from .decode import (decode, Insn, NORMAL, BRANCH, JUMP, JUMP_IND, CALL,
                     CALL_IND, RET, INVALID)


@dataclass
class Block:
    start: int
    end: int = 0                      # exclusive, includes any delay slot
    insns: list = field(default_factory=list)
    succs: set = field(default_factory=set)


@dataclass
class Function:
    start: int
    blocks: dict = field(default_factory=dict)
    callers: set = field(default_factory=set)
    callees: set = field(default_factory=set)
    unresolved: list = field(default_factory=list)  # indirect jumps we could not follow

    @property
    def end(self):
        return max(b.end for b in self.blocks.values()) if self.blocks else self.start

    def size(self):
        return sum(b.end - b.start for b in self.blocks.values())


class Image:
    """Flat view over the address ranges that hold SH-2 code.

    `norm` collapses the SH-2 cache-region mirrors (the same memory is visible
    at several addresses depending on the top three bits) onto one canonical
    address, so a pointer fetched as 0x22000000-based resolves like 0x02000000.
    """

    def __init__(self, norm=None):
        self.spans = []   # (base, bytes)
        self.norm = norm or (lambda a: a)

    def add(self, base: int, data: bytes):
        self.spans.append((base, data))

    def _find(self, addr):
        addr = self.norm(addr)
        for base, data in self.spans:
            if base <= addr < base + len(data):
                return base, data
        return None

    def readable(self, addr, n=2):
        s = self._find(addr)
        return s is not None and self.norm(addr) + n <= s[0] + len(s[1])

    def u16(self, addr):
        base, data = self._find(addr)
        o = self.norm(addr) - base
        return int.from_bytes(data[o:o + 2], "big")

    def u32(self, addr):
        base, data = self._find(addr)
        o = self.norm(addr) - base
        return int.from_bytes(data[o:o + 4], "big")

    def s16(self, addr):
        v = self.u16(addr)
        return v - 0x10000 if v & 0x8000 else v


class Analyzer:
    def __init__(self, image: Image, is_code_addr=None):
        self.img = image
        self.funcs = {}
        self.code = set()        # every address that is the first byte of an insn
        self.data = set()        # literal-pool words
        self.pool_refs = defaultdict(set)
        self.xrefs = defaultdict(set)
        self.tables = {}      # dispatch address -> recovered target list
        # Which addresses are plausible code targets — lets us reject junk.
        self.is_code_addr = is_code_addr or (lambda a: image.readable(a, 2))
        self._pending = []

    # ------------------------------------------------------------------
    def add_entry(self, addr, why=""):
        if addr & 1:
            return
        if self.is_code_addr(addr):
            self._pending.append((addr, why))

    def run(self, max_funcs=100000):
        seen = set()
        while self._pending:
            addr, why = self._pending.pop()
            if addr in seen or len(self.funcs) >= max_funcs:
                continue
            seen.add(addr)
            try:
                self._walk_function(addr)
            except (TypeError, IndexError, KeyError):
                # Ran off the end of a mapped span — the target was not code.
                self.funcs.pop(addr, None)
        return self.funcs

    # ------------------------------------------------------------------
    def _indirect_target(self, ins, regs):
        """Resolve a register-indirect transfer, if the register is known.

        `jmp`/`jsr` take an absolute address, but `braf`/`bsrf` add the register
        to PC+4 — that is how SH-2 code reaches anything outside a 12-bit
        displacement, so it is the dominant far-call form.
        """
        if ins.rn is None:
            return None
        v = regs.get(ins.rn)
        if v is None:
            return None
        if ins.mnem in ("braf", "bsrf"):
            return (ins.addr + 4 + v) & 0xFFFFFFFF
        return v & 0xFFFFFFFF

    # ------------------------------------------------------------------
    def _recover_jump_table(self, ins, blk):
        """Recover the targets of a table-driven jump.

        Both dispatch idioms this game uses start by materialising the table
        base with `mova` and then indexing it:

            mova  @(d,pc),r0        ! table base
            mov.l @(r0,rM),rN       ! absolute entry   -> jmp @rN
            mova  @(d,pc),r0
            mov.w @(r0,rM),r0       ! 16-bit offset    -> bsrf/braf r0

        So we scan back through the block for the `mova` and the indexed load
        that feeds the transfer register, then read entries until one stops
        looking like code.
        """
        base = None
        entry_size = None
        want = ins.rn
        for prev in reversed(blk.insns[:-1]):
            if entry_size is None:
                # mov.l @(r0,rM),rN = 0x0nmE ; mov.w @(r0,rM),rN = 0x0nmD
                hi = prev.word & 0xF00F
                if hi in (0x000E, 0x000D) and prev.rn == want:
                    entry_size = 4 if hi == 0x000E else 2
                    continue
                if prev.writes and want in [w for w in prev.writes if isinstance(w, int)]:
                    return []          # register redefined some other way
                continue
            if prev.mnem == "mova":
                base = prev.imm
                break
        if base is None or entry_size is None:
            return []

        targets = []
        for i in range(512):
            a = base + i * entry_size
            if not self.img.readable(a, entry_size):
                break
            if entry_size == 4:
                tgt = self.img.u32(a)
            else:
                tgt = (ins.addr + 4 + self.img.s16(a)) & 0xFFFFFFFF
            if not self.is_code_addr(tgt) or not self.img.readable(tgt, 2):
                break
            if decode(self.img.u16(tgt), tgt).kind == INVALID:
                break
            # The table cannot extend into code we already decoded.
            if a in self.code:
                break
            targets.append(tgt)
            for k in range(entry_size):
                self.data.add(a + k)
        return targets

    # ------------------------------------------------------------------
    def _walk_function(self, entry):
        if entry in self.funcs:
            return self.funcs[entry]
        fn = Function(start=entry)
        self.funcs[entry] = fn

        worklist = [entry]
        visited = set()
        while worklist:
            baddr = worklist.pop()
            if baddr in visited or not self.img.readable(baddr, 2):
                continue
            visited.add(baddr)
            blk = Block(start=baddr)
            regs = {}          # register -> known constant, within this block
            addr = baddr
            guard = 0

            while True:
                guard += 1
                if guard > 20000 or not self.img.readable(addr, 2):
                    break
                ins = decode(self.img.u16(addr), addr)
                self.code.add(addr)
                blk.insns.append(ins)

                # Resolve a literal-pool load and remember the value.
                if ins.pool:
                    paddr, psz = ins.pool
                    if self.img.readable(paddr, psz):
                        val = self.img.u32(paddr) if psz == 4 else self.img.s16(paddr)
                        regs[ins.rn] = val
                        for k in range(psz):
                            self.data.add(paddr + k)
                        self.pool_refs[paddr].add(addr)
                elif ins.mnem == "mov" and ins.imm is not None and ins.rn is not None:
                    regs[ins.rn] = ins.imm
                elif ins.mnem == "mova":
                    regs[0] = ins.imm
                elif ins.writes:
                    for w in ins.writes:
                        if isinstance(w, int):
                            regs.pop(w, None)

                if ins.kind == INVALID:
                    blk.end = addr + 2
                    break

                # A delayed branch executes the following instruction first.
                if ins.delay and self.img.readable(addr + 2, 2):
                    ds = decode(self.img.u16(addr + 2), addr + 2)
                    self.code.add(addr + 2)
                    blk.insns.append(ds)
                    if ds.pool:
                        paddr, psz = ds.pool
                        if self.img.readable(paddr, psz):
                            for k in range(psz):
                                self.data.add(paddr + k)
                            self.pool_refs[paddr].add(addr + 2)
                    end = addr + 4
                else:
                    end = addr + 2

                if ins.kind in (BRANCH,):
                    blk.end = end
                    self.xrefs[ins.target].add(addr)
                    blk.succs |= {ins.target, end}
                    worklist += [ins.target, end]
                    break
                if ins.kind == JUMP:
                    blk.end = end
                    self.xrefs[ins.target].add(addr)
                    blk.succs.add(ins.target)
                    worklist.append(ins.target)
                    break
                if ins.kind == CALL:
                    self.xrefs[ins.target].add(addr)
                    fn.callees.add(ins.target)
                    self.add_entry(ins.target, "bsr")
                    addr = end
                    continue
                if ins.kind == CALL_IND:
                    tgt = self._indirect_target(ins, regs)
                    if tgt is not None and self.is_code_addr(tgt):
                        fn.callees.add(tgt)
                        self.xrefs[tgt].add(addr)
                        self.add_entry(tgt, ins.mnem)
                    else:
                        tbl = self._recover_jump_table(ins, blk)
                        if tbl:
                            self.tables[addr] = tbl
                            for t in tbl:
                                fn.callees.add(t)
                                self.xrefs[t].add(addr)
                                self.add_entry(t, "table")
                        else:
                            fn.unresolved.append((addr, ins.mnem))
                    addr = end
                    continue
                if ins.kind == JUMP_IND:
                    blk.end = end
                    tgt = self._indirect_target(ins, regs)
                    if tgt is not None and self.is_code_addr(tgt):
                        # A tail call through a register: treat as its own function.
                        fn.callees.add(tgt)
                        self.xrefs[tgt].add(addr)
                        self.add_entry(tgt, "jmp")
                    else:
                        tbl = self._recover_jump_table(ins, blk)
                        if tbl:
                            self.tables[addr] = tbl
                            for t in tbl:
                                fn.callees.add(t)
                                self.xrefs[t].add(addr)
                                self.add_entry(t, "table")
                        else:
                            fn.unresolved.append((addr, ins.mnem))
                    break
                if ins.kind == RET:
                    blk.end = end
                    break
                addr = end

            if blk.end == 0:
                blk.end = addr + 2
            fn.blocks[baddr] = blk
        return fn

    # ------------------------------------------------------------------
    def stats(self):
        code_bytes = len(self.code) * 2
        return {
            "functions": len(self.funcs),
            "code_insns": len(self.code),
            "code_bytes": code_bytes,
            "pool_bytes": len(self.data),
            "unresolved": sum(len(f.unresolved) for f in self.funcs.values()),
        }
