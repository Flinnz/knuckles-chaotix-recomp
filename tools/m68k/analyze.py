"""Recursive-descent code discovery for the 68000 side of the cartridge.

Everything here works in **ROM offset space**, not in any one CPU view. The
68000 sees the same cartridge bytes at more than one address — the reset entry
is named as raw offset 0x3F0 while the exception vectors point into the
0x880000 window the 32X adapter provides — so offsets are the only stable
coordinate. Relative branches are then self-consistent by construction, and
absolute operands stay untouched in the emitted listing, which is what lets it
reassemble byte for byte.

Unlike the SH-2 there are no delay slots, but instructions are variable length,
so a wrong length does not merely mis-print one line: it desynchronises the
stream from that point on. Discovery is therefore strictly reachability-driven.
"""

import re
from collections import defaultdict
from dataclasses import dataclass, field

from .decode import (decode, BRANCH, JUMP, JUMP_IND, CALL, CALL_IND, RET,
                     TRAP, INVALID)

# Windows through which the 68000 addresses the cartridge.
WIN_DIRECT = (0x000000, 0x400000)     # raw cartridge, before the adapter maps it
WIN_FIXED = (0x880000, 0x900000)      # first 512 KB, always present
WIN_BANKED = (0x900000, 0xA00000)     # 1 MB banked window

# Branch-style mnemonics compute their target from the PC, so it is already an
# offset. Everything else names an absolute 68000 address.
RELATIVE = ("bra", "bsr", "b", "db")


@dataclass
class Block:
    start: int
    end: int = 0
    insns: list = field(default_factory=list)
    succs: set = field(default_factory=set)


@dataclass
class Function:
    start: int
    blocks: dict = field(default_factory=dict)
    unresolved: list = field(default_factory=list)

    def size(self):
        return sum(b.end - b.start for b in self.blocks.values())


def to_offset(addr, rom_size):
    """Translate a 68000 address to a cartridge offset, or None."""
    if WIN_FIXED[0] <= addr < WIN_FIXED[1]:
        off = addr - WIN_FIXED[0]
    elif WIN_BANKED[0] <= addr < WIN_BANKED[1]:
        # The banked window's contents depend on a bank register written at run
        # time; bank 0 is the only statically knowable mapping.
        off = addr - WIN_BANKED[0]
    elif WIN_DIRECT[0] <= addr < WIN_DIRECT[1]:
        off = addr
    else:
        return None
    return off if 0 <= off < rom_size else None


class Analyzer:
    def __init__(self, data: bytes):
        self.data = data
        self.insns = {}
        self.code = set()
        self.leaders = set()
        self.func_entries = set()
        self.block_ends = {}
        self.data_marks = {}
        self.xrefs = defaultdict(set)
        self.entry_reasons = {}
        self.indirect = []
        self.tables = {}
        self._todo = []
        self._traced = set()

    # ------------------------------------------------------------------
    def fetch(self, a):
        if a < 0 or a + 2 > len(self.data):
            raise IndexError(a)
        return int.from_bytes(self.data[a:a + 2], "big")

    def readable(self, a, n=2):
        return 0 <= a and a + n <= len(self.data)

    def mark_data(self, off, kind, size):
        for k in range(size):
            self.data_marks[off + k] = kind if k == 0 else None

    def add_function(self, off, why=""):
        if off is None or off & 1 or not self.readable(off, 2):
            return False
        self.func_entries.add(off)
        self.entry_reasons.setdefault(off, why)
        self._add_block(off)
        return True

    def _add_block(self, off):
        if off is None or off & 1 or not self.readable(off, 2):
            return
        self.leaders.add(off)
        if off not in self._traced:
            self._todo.append(off)

    def _target_offset(self, ins):
        """Resolve a control-transfer target to a cartridge offset."""
        if ins.target is None:
            return None
        if ins.mnem.startswith(RELATIVE):
            return ins.target          # already an offset: PC-relative
        return to_offset(ins.target, len(self.data))

    # ------------------------------------------------------------------
    def run(self):
        while self._todo:
            self._trace(self._todo.pop())
        self._build()
        return self.funcs

    def _trace(self, start):
        if start in self._traced:
            return
        self._traced.add(start)
        off = start
        recent = []
        while True:
            if not self.readable(off, 2):
                return
            if off != start and off in self.leaders:
                self._add_block(off)
                return
            try:
                ins = self.insns.get(off) or decode(self.fetch, off)
            except IndexError:
                return
            if not self.readable(off, ins.size):
                return
            self.insns[off] = ins
            self.code.add(off)
            recent.append(ins)
            del recent[:-16]
            end = off + ins.size

            if ins.kind == INVALID:
                self.block_ends[end] = (INVALID, None)
                return
            if ins.kind == BRANCH:
                t = self._target_offset(ins)
                self.block_ends[end] = (BRANCH, t)
                if t is not None:
                    self.xrefs[t].add(off)
                    self._add_block(t)
                self._add_block(end)
                return
            if ins.kind == JUMP:
                t = self._target_offset(ins)
                self.block_ends[end] = (JUMP, t)
                if t is not None:
                    self.xrefs[t].add(off)
                    self._add_block(t)
                return
            # A call's return address has to be a block of its own. Nothing in
            # the listing needs that, but a recompiler does: `rts` hands the
            # popped address to the trampoline, which can only enter at a block
            # it has a row for. The SH-2 side found the same thing.
            if ins.kind == CALL:
                t = self._target_offset(ins)
                if t is not None:
                    self.xrefs[t].add(off)
                    self.add_function(t, ins.mnem)
                self._add_block(end)
                off = end
                continue
            if ins.kind == CALL_IND:
                if not self._dispatch(ins, recent):
                    self.indirect.append((off, ins.mnem))
                self._add_block(end)
                off = end
                continue
            if ins.kind == JUMP_IND:
                self.block_ends[end] = (JUMP_IND, None)
                if not self._dispatch(ins, recent):
                    self.indirect.append((off, ins.mnem))
                return
            if ins.kind == RET:
                self.block_ends[end] = (RET, None)
                return
            off = end

    def _dispatch(self, ins, recent):
        """Follow a recovered dispatch table, if this transfer is one."""
        entries = self._recover_pcrel_table(ins, recent)
        if not entries:
            return False
        self.tables[ins.addr] = entries
        for a in entries:
            self.xrefs[a].add(ins.addr)
            self._add_block(a)
        return True

    # ------------------------------------------------------------------
    def _index_stride(self, recent, ireg):
        """Work out the element size of a table indexed by data register `ireg`.

        The index is scaled by ordinary arithmetic before the jump, so the
        stride is recoverable from it: each `add %dn,%dn` or shift doubles it,
        and an `andi` mask sets the granularity from its lowest set bit
        (`andi #120` keeps bits 3..6, so values step by 8).
        """
        stride = 1
        pat_reg = f"%d{ireg}"
        for ins in reversed(recent[:-1]):
            ops, mnem = ins.ops, ins.mnem
            if not ops.endswith(pat_reg):
                continue
            if mnem in ("addw", "addl") and ops == f"{pat_reg},{pat_reg}":
                stride *= 2
                continue
            if mnem[:3] in ("lsl", "asl") and ops.startswith("#"):
                try:
                    stride *= 1 << int(ops[1:ops.index(",")])
                except ValueError:
                    pass
                continue
            if mnem.startswith("andi") and ops.startswith("#"):
                try:
                    mask = int(ops[1:ops.index(",")])
                except ValueError:
                    break
                if mask > 0:
                    stride *= (mask & -mask)
                break
            break                       # any other definition ends the chain
        return max(stride, 2)

    def _slot_size(self, a, limit=8):
        """Bytes of one dispatch-table slot at `a`, or None if it is not one.

        A slot holds a single control transfer, with `nop` allowed to pad it out
        to the stride. `rts` counts: a table whose nth case is "do nothing" is
        written as `nop / rts`, which is what the controller identification
        table at 0x8F45D0 is half made of.
        """
        n = 0
        while n < limit:
            if not self.readable(a + n, 2):
                return None
            try:
                e = decode(self.fetch, a + n)
            except IndexError:
                return None
            if not self.readable(a + n, e.size) or n + e.size > limit:
                return None
            if e.mnem == "nop":
                n += e.size
                continue
            if e.kind not in (JUMP, BRANCH, CALL, RET):
                return None
            if e.kind != RET:
                t = self._target_offset(e)
                if t is None or not self.readable(t, 2):
                    return None
            return n + e.size
        return None

    def _recover_pcrel_table(self, ins, recent):
        """Recover `jmp/jsr %pc@(base,%dn:w)` — the 68000 dispatch idiom here.

        Each entry is a single control-transfer instruction padded out to the
        stride, so the table self-terminates: walk entries until one stops being
        one. That matches the explicit `cmpi #n` bounds check these sites carry,
        without having to find it.

        The stride is read off the arithmetic that scaled the index — except
        where there is none, because the index was already scaled when it was
        stored and the site just loads it. `0x8834D6` is that: `move.w
        ($ffdfe0),d0` and straight into the `jsr`, so the chain ends on an
        unknown definition and the floor of 2 stands. Slot zero settles it
        without guessing, since an entry has to fit in its own slot and slot
        zero there is a 4-byte `bra.w`. That table is the mode dispatch the
        engine runs every frame and its six cases were all missing.
        """
        m = re.match(r"%pc@\(0x([0-9a-f]+),%d(\d):", ins.ops)
        if not m:
            return []
        base = int(m.group(1), 16)
        first = self._slot_size(base)
        if first is None:
            # Some of these origins are the dispatching instruction itself, so
            # slot zero is the `jmp` and no index ever selects it: 0x8811D6 is
            # `jmp %pc@(0x8811d6,%d0:w)` with its three cases starting at
            # 0x8811DA. The emitter already knows origins overlap here — it
            # demotes the instruction they land inside to raw bytes. Step over
            # it rather than giving the table up.
            if not base <= ins.addr:
                return []
            base = ins.addr + ins.size
            first = self._slot_size(base)
            if first is None:
                return []
        stride = max(self._index_stride(recent, int(m.group(2))), first)
        targets = []
        for i in range(256):
            a = base + i * stride
            n = self._slot_size(a, stride)
            if n is None:
                break
            targets.append(a)
        return targets

    # ------------------------------------------------------------------
    # The scale field of a brief extension word is a 68020 addition that a
    # 68000 ignores, so nothing assembled for this CPU ever sets it — which
    # makes it a reliable sign that the bytes are not code. It also happens to
    # be the one thing `m68k-elf-as` refuses outright for -m68000, so a
    # misclassification carrying it fails the round-trip rather than passing
    # quietly; both of the ones this caught were data, an offset table at
    # 0x0771FC and the bytes after an `rts` at 0x003E5E.
    _SCALED = re.compile(r":[wl]:[248]")

    def looks_like_code(self, off, max_insns=64, min_insns=4):
        """Conservative test that `off` begins a real instruction sequence."""
        if off is None or off & 1 or not self.readable(off, 2):
            return False
        a = off
        for i in range(max_insns):
            if not self.readable(a, 2):
                return False
            try:
                ins = decode(self.fetch, a)
            except IndexError:
                return False
            if ins.kind == INVALID or not self.readable(a, ins.size):
                return False
            if self._SCALED.search(ins.ops):
                return False
            if ins.kind in (BRANCH, JUMP, CALL):
                t = self._target_offset(ins)
                if t is None or not self.readable(t, 2):
                    return False
            if ins.kind in (RET, JUMP, JUMP_IND):
                return i + 1 >= min_insns
            a += ins.size
        return False

    def scan_after_returns(self):
        """What follows an `rts`: unreachable by fall-through, so seed it.

        The SH-2 side seeds after an unconditional `bra`/`jmp` as well, on the
        same reasoning, and that does not carry over here. Tried, it adds 73
        functions and breaks the whole-cartridge round-trip — because on the
        68000 what sits behind a `jmp` is very often the table it dispatches
        through, and a wrong start on a variable-length encoding costs far more
        than it does on a fixed-width one. The gate is the point; leave it.
        """
        added = 0
        for end, (kind, _) in list(self.block_ends.items()):
            if kind != RET or end in self.code or end in self.data_marks:
                continue
            if ((self.lone_rte(end) or self.looks_like_code(end))
                    and self.add_function(end, "after rts")):
                added += 1
        return added

    def lone_rte(self, off):
        """A single `rte` is a whole interrupt handler, and `looks_like_code`
        wants four instructions before it will believe a blind sweep.

        The evidence here is better than length. `rte` cannot appear anywhere
        but as the last instruction of an exception handler — nothing falls into
        one and no ordinary routine ends in one — so an `rte` sitting where
        control cannot fall through is a handler by construction. 0x880B2A is
        exactly that, one instruction between an `rts` and the reserved-vector
        handler the cartridge header names, and it is what the 68000 runs if a
        vertical interrupt arrives before the engine has installed its own.
        """
        if off is None or off & 1 or not self.readable(off, 2):
            return False
        try:
            ins = decode(self.fetch, off)
        except IndexError:
            return False
        return ins.mnem == "rte"

    # An address the engine puts somewhere for something to jump through later.
    _INSTALL_SRC = re.compile(r"^%pc@\(0x([0-9a-f]+)\),%(a\d|fp|sp)$")
    _INSTALL_DST = re.compile(r"^%(a\d|fp|sp),0x([0-9a-f]+)$")
    _WRITES_AREG = re.compile(r",%(a\d|fp|sp)(@[+-]?)?$|%(a\d|fp|sp)@[+-]")

    def scan_installed_handlers(self):
        """Entry points the engine names with a PC-relative `lea` and stores.

        The interrupt handlers are reached through none of the transfers
        discovery follows. The adapter's vector points at a stub in the
        cartridge, the stub is `jmp` through a fixed word of work RAM, and the
        engine writes the real address into that word while it runs:

            883464  lea  %pc@(0x8836d2),%a0
            883468  move.l %a0,($ffc032)        ; the vertical interrupt's slot

        So nothing static reaches 0x8836D2, and the whole vertical interrupt
        handler — which calls the comm-0 poster, the VDP updater and the pad
        reader, every frame — was missing from the front end. It is the same
        shape for every handler the engine swaps in, and there are twelve
        different ones written to that one slot.

        The `lea` computes its address PC-relative, so the target is already a
        plain cartridge offset and nothing has to be inferred. The pair has to
        be in one unbroken run of instructions with the register untouched
        between them, and the target has to pass `looks_like_code`, which is the
        same bar `scan_after_returns` clears.
        """
        added = 0
        held, prev_end = {}, None
        for off in sorted(self.code):
            ins = self.insns[off]
            if off != prev_end:
                held.clear()
            prev_end = off + ins.size

            if ins.mnem == "lea":
                m = self._INSTALL_SRC.match(ins.ops)
                if m:
                    held[m.group(2)] = int(m.group(1), 16)
                    continue
            if ins.mnem == "movel":
                m = self._INSTALL_DST.match(ins.ops)
                if m and m.group(1) in held:
                    t = held.pop(m.group(1))
                    # Two instructions is enough here where four is the bar for
                    # a blind sweep, because the naming carries the weight the
                    # length was standing in for. Interrupt handlers are short:
                    # 0x883710 is `move.b #1,($ffffd1)` and `rte`, and 0x883BA8
                    # — which the engine runs 1,408 times in 2,000 frames — is
                    # three instructions. Both were being turned away.
                    if (self.looks_like_code(t, min_insns=2)
                            and self.add_function(t, "installed")):
                        added += 1
                    continue
            # Anything else that touches an address register ends its chain.
            if ins.mnem.startswith("movem"):
                held.clear()
            else:
                for m in self._WRITES_AREG.finditer(ins.ops):
                    held.pop(m.group(1) or m.group(3), None)
        return added

    # ------------------------------------------------------------------
    def _build(self):
        self.blocks = {}
        cur = None
        for off in sorted(self.code):
            contiguous = cur is not None and off == cur.end
            if cur is not None and (not contiguous or off in self.leaders):
                if contiguous:
                    cur.succs.add(off)
                cur = None
            if cur is None:
                cur = Block(start=off)
                self.blocks[off] = cur
            ins = self.insns[off]
            cur.insns.append(ins)
            cur.end = off + ins.size
            if cur.end in self.block_ends:
                kind, target = self.block_ends[cur.end]
                if kind == BRANCH:
                    cur.succs |= {t for t in (target, cur.end) if t is not None}
                elif kind == JUMP and target is not None:
                    cur.succs.add(target)
                cur = None

        self.funcs = {}
        for entry in sorted(self.func_entries):
            if entry not in self.blocks:
                continue
            fn = Function(start=entry)
            stack, seen = [entry], set()
            while stack:
                b = stack.pop()
                if b in seen or b not in self.blocks:
                    continue
                if b != entry and b in self.func_entries:
                    continue
                seen.add(b)
                blk = self.blocks[b]
                fn.blocks[b] = blk
                stack.extend(blk.succs)
            self.funcs[entry] = fn
        return self.funcs

    def stats(self):
        return {
            "functions": len(getattr(self, "funcs", {})),
            "blocks": len(getattr(self, "blocks", {})),
            "insns": len(self.code),
            "code_bytes": sum(self.insns[a].size for a in self.code),
            "unresolved": len(self.indirect),
        }
