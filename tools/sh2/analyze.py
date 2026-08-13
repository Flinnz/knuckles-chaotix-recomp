"""Recursive-descent code discovery for SH-2.

SH-2 cannot encode a 32-bit constant inline, so compilers park constants and
code addresses in literal pools interleaved with the instruction stream and
reach them via `mov.l @(disp,pc),Rn`. Two consequences drive this module:

  1. Those pool words sit *inside* functions and must not be disassembled as
     code, so a plain linear sweep produces garbage.
  2. Almost every non-local call is `mov.l @(disp,pc),Rn` + `jsr @Rn`, or the
     PC-relative `bsrf`, so tracking literal loads recovers the call graph.

Discovery runs in two phases. Phase one traces reachable instructions and
records basic-block leaders; phase two cuts the instruction stream at those
leaders to build blocks and group them into functions. Doing it in that order
matters: a backward branch into the middle of an already-traced run has to
*split* that run, and a single-pass walk would instead emit two overlapping
copies of the same code.
"""

from collections import defaultdict
from dataclasses import dataclass, field

from .decode import (decode, BRANCH, JUMP, JUMP_IND, CALL, CALL_IND, RET,
                     INVALID)

MASK32 = 0xFFFFFFFF


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
    callees: set = field(default_factory=set)
    unresolved: list = field(default_factory=list)

    @property
    def end(self):
        return max(b.end for b in self.blocks.values()) if self.blocks else self.start

    def size(self):
        return sum(b.end - b.start for b in self.blocks.values())


@dataclass
class Table:
    """A recovered dispatch table."""
    dispatch: int
    base: int
    kind: str          # 'long' absolute | 'word'/'byte' offset from dispatch+4
    entries: list      # (address_of_entry, target)


class Image:
    """Flat view over the address ranges that hold SH-2 code.

    `norm` collapses the SH-2 cached/cache-through mirrors onto one canonical
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

    def _slice(self, addr, n):
        base, data = self._find(addr)
        o = self.norm(addr) - base
        return data[o:o + n]

    def u8(self, addr):
        return self._slice(addr, 1)[0]

    def u16(self, addr):
        return int.from_bytes(self._slice(addr, 2), "big")

    def u32(self, addr):
        return int.from_bytes(self._slice(addr, 4), "big")

    def s16(self, addr):
        v = self.u16(addr)
        return v - 0x10000 if v & 0x8000 else v


class Analyzer:
    def __init__(self, image: Image, is_code_addr=None):
        self.img = image
        self.insns = {}                  # addr -> Insn, the authoritative decode
        self.code = set()                # instruction start addresses
        self.leaders = set()             # basic-block leaders
        self.func_entries = set()
        self.block_ends = {}             # end address -> (kind, target) of its branch
        self.data = {}                   # addr -> ('long'|'short'|'byte')
        self.data_firm = set()           # data proven by an instruction reference
        self.tables = {}                 # dispatch addr -> Table
        self.xrefs = defaultdict(set)
        self.entry_reasons = {}
        self.indirect = []               # (addr, mnem) we could not resolve
        self.is_code_addr = is_code_addr or (lambda a: image.readable(a, 2))
        self._blocks_todo = []
        self._traced = set()

    # -- seeding -------------------------------------------------------
    def add_function(self, addr, why=""):
        addr &= MASK32
        if addr & 1 or not self.is_code_addr(addr):
            return False
        self.func_entries.add(addr)
        self.entry_reasons.setdefault(addr, why)
        self._add_block(addr)
        return True

    def _add_block(self, addr):
        addr &= MASK32
        if addr & 1 or not self.is_code_addr(addr):
            return
        self.leaders.add(addr)
        if addr not in self._traced:
            self._blocks_todo.append(addr)

    # -- phase 1: trace ------------------------------------------------
    def run(self):
        while True:
            while self._blocks_todo:
                self._trace(self._blocks_todo.pop())
            if not self._retry_indirect():
                break
        self._build_functions()
        return self.funcs

    def _retry_indirect(self):
        """Re-attempt unresolved dispatches now that more code is decoded."""
        added = 0
        still = []
        for addr, mnem in self.indirect:
            ins = self.insns.get(addr)
            tbl = self._recover_table(ins, []) if ins else None
            if tbl:
                self.tables[addr] = tbl
                for _, tgt in tbl.entries:
                    self.xrefs[tgt].add(addr)
                    if self.add_function(tgt, "table"):
                        added += 1
            else:
                still.append((addr, mnem))
        self.indirect = still
        return added

    def mark_data(self, addr, kind, size, firm=True):
        """Record a span as data, so the emitter never disassembles it."""
        self._mark_data(addr, kind, size, firm)

    def _mark_data(self, addr, kind, size, firm=True):
        for k in range(size):
            self.data[addr + k] = kind if k == 0 else None
            if firm:
                self.data_firm.add(addr + k)

    def _trace(self, start):
        if start in self._traced:
            return
        self._traced.add(start)
        addr = start
        regs = {}          # register -> known constant, within this straight run
        recent = []        # sliding window, for recognising dispatch idioms

        while True:
            if not self.img.readable(addr, 2):
                return
            # Reaching another leader ends this run; it continues as its own block.
            if addr != start and addr in self.leaders:
                self._add_block(addr)
                return

            ins = self.insns.get(addr) or decode(self.img.u16(addr), addr)
            self.insns[addr] = ins
            self.code.add(addr)
            recent.append(ins)
            del recent[:-16]

            self._track_const(ins, regs)
            self._note_computed_pr(ins, regs)

            if ins.kind == INVALID:
                self.block_ends[addr + 2] = (INVALID, None)
                return

            end = addr + 2
            if ins.delay and self.img.readable(addr + 2, 2):
                ds = self.insns.get(addr + 2) or decode(self.img.u16(addr + 2), addr + 2)
                self.insns[addr + 2] = ds
                self.code.add(addr + 2)
                if ds.pool:
                    self._note_pool(ds)
                # A delay slot runs with the register state the branch saw, so
                # the constants tracked up to here are the right ones — and
                # `lds Rn,pr` in the delay slot of a `bra` is exactly how this
                # engine arranges a computed return.
                self._note_computed_pr(ds, regs)
                end = addr + 4

            if ins.kind == BRANCH:
                self.block_ends[end] = (BRANCH, ins.target)
                self.xrefs[ins.target].add(addr)
                self._add_block(ins.target)
                self._add_block(end)
                return
            if ins.kind == JUMP:
                self.block_ends[end] = (JUMP, ins.target)
                self.xrefs[ins.target].add(addr)
                self._add_block(ins.target)
                return
            if ins.kind == CALL:
                self.xrefs[ins.target].add(addr)
                self.add_function(ins.target, "bsr")
                self.block_ends[end] = (CALL, ins.target)
                self._add_block(end)
                return
            if ins.kind == CALL_IND:
                self._resolve_indirect(ins, regs, recent, is_call=True)
                self.block_ends[end] = (CALL_IND, None)
                self._add_block(end)
                return
            if ins.kind == JUMP_IND:
                self.block_ends[end] = (JUMP_IND, None)
                self._resolve_indirect(ins, regs, recent, is_call=False)
                return
            if ins.kind == RET:
                self.block_ends[end] = (RET, None)
                return
            addr = end

    def _note_computed_pr(self, ins, regs):
        """A return address the code computes, rather than one a call leaves.

        `bsr`/`jsr` write PR themselves and the instruction after them is
        already a leader. `lds Rn,pr` with Rn holding a literal is the other way
        to set one, and it is invisible to everything else here: the address it
        names is usually *inside* an existing block, so no branch target, no
        table entry and no sweep will ever produce it.

        This game has one, and it is what stopped the run after the title
        screen. At 0x06000AA6 a command handler loads the literal 0x06000A64 and
        puts it in PR in the delay slot of a `bra`, so the routine it
        tail-branches to returns into the middle of another handler's block. The
        runtime dispatches by block address, found nothing there, and parked the
        master at zero. M3 recorded this shape as the one hole in the PR model
        and said no site was known in this game; this is the site.

        `lds.l @Rn+,pr` is deliberately not included — that is a handler
        restoring PR from the stack, and what it holds is not a literal.
        """
        if ins.mnem != "lds" or "pr" not in ins.writes or ins.rn is None:
            return
        v = regs.get(ins.rn)
        if v is None or not self.is_code_addr(v):
            return
        self.xrefs[v].add(ins.addr)
        self.add_function(v, "computed pr")

    def _note_pool(self, ins):
        paddr, psz = ins.pool
        if self.img.readable(paddr, psz):
            self._mark_data(paddr, "long" if psz == 4 else "short", psz)
            self.xrefs[paddr].add(ins.addr)

    def _track_const(self, ins, regs):
        if ins.pool:
            paddr, psz = ins.pool
            if self.img.readable(paddr, psz):
                self._note_pool(ins)
                regs[ins.rn] = self.img.u32(paddr) if psz == 4 else self.img.s16(paddr)
                return
        if ins.mnem == "mov" and ins.imm is not None and ins.rn is not None:
            regs[ins.rn] = ins.imm
        elif ins.mnem == "mova":
            regs[0] = ins.imm
        else:
            for w in ins.writes:
                if isinstance(w, int):
                    regs.pop(w, None)

    # -- indirect transfers --------------------------------------------
    def _resolve_indirect(self, ins, regs, recent, is_call):
        v = regs.get(ins.rn) if ins.rn is not None else None
        if v is not None:
            # jmp/jsr take an absolute address; braf/bsrf add the register to PC+4.
            tgt = (ins.addr + 4 + v) & MASK32 if ins.mnem in ("braf", "bsrf") else v & MASK32
            if self.is_code_addr(tgt):
                self.xrefs[tgt].add(ins.addr)
                self.add_function(tgt, ins.mnem)
                return
        tbl = self._recover_table(ins, recent)
        if tbl:
            self.tables[ins.addr] = tbl
            for entry_addr, tgt in tbl.entries:
                self.xrefs[tgt].add(ins.addr)
                self.add_function(tgt, "table")
            return
        self.indirect.append((ins.addr, ins.mnem))

    def _backward_window(self, addr, n=16):
        """The instructions immediately preceding `addr` in memory.

        The dispatch idioms are a fixed *layout* — `mova` then an indexed load
        then the branch — so the window must come from the instruction stream,
        not from the trace path. Otherwise a table is recoverable or not
        depending on which edge discovery happened to arrive on.
        """
        out = []
        a = addr - 2
        while len(out) < n and a >= 0 and a in self.code:
            out.append(self.insns[a])
            a -= 2
        out.reverse()
        return out

    def _recover_table(self, ins, recent):
        """Recognise the three dispatch idioms this game uses.

        A  mova Lbase,r0 ; mov.l @(r0,rM),rN ; jmp/jsr @rN     32-bit absolute
        B  mova Lbase,r0 ; mov.w @(r0,rM),rN ; braf/bsrf rN    16-bit offsets
        C  mov.l Lp,rB   ; mov.b @(r0,rB),rT ; braf/bsrf rT     8-bit offsets

        In A and B `mova` gives the table address exactly. In C the literal is
        an indexing origin that sits before the real entries, so instead we use
        the fact that the entries are laid out immediately after the dispatch's
        delay slot.
        """
        want = ins.rn
        if want is None:
            return None
        window = self._backward_window(ins.addr)
        if len(window) < len(recent) - 1:
            window = recent[:-1]
        load = None
        for prev in reversed(window):
            hi = prev.word & 0xF00F
            if hi in (0x000E, 0x000D, 0x000C) and prev.rn == want:
                load = {0x000E: "long", 0x000D: "word", 0x000C: "byte"}[hi]
                break
            if want in [w for w in prev.writes if isinstance(w, int)]:
                return None          # register redefined some other way
        if load is None:
            return None

        base = None
        for prev in reversed(window):
            if prev.mnem == "mova":
                base = prev.imm
                break
        if load == "byte" or base is None:
            # Offset tables are emitted straight after the branch's delay slot.
            base = ins.addr + 4
        if not self.is_code_addr(base) and not self.img.readable(base, 1):
            return None

        size = {"long": 4, "word": 2, "byte": 1}[load]
        entries = []
        empty = []           # null slots confirmed by a later real entry
        pending = []         # ...and ones not confirmed yet
        limit = None
        a = base
        for _ in range(1024):
            if not self.img.readable(a, size) or a in self.code:
                break
            if limit is not None and a >= limit:
                break
            if load == "long":
                tgt = self.img.u32(a)
            elif load == "word":
                tgt = (ins.addr + 4 + self.img.s16(a)) & MASK32
            else:
                tgt = (ins.addr + 4 + self.img.u8(a)) & MASK32
            # A null slot is an empty case, not the end of the table: the case
            # that does nothing is written as a zero and the entries after it
            # are still entries. The table at 0x06000560 has one, and the entry
            # past it — 0x06000584 — is what the special stage transfers to and
            # what killed the master with no block. Kept only once a real entry
            # follows, and only a couple in a row, so a table cannot run off
            # into a region of zeros.
            if load == "long" and tgt == 0 and entries and len(pending) < 2:
                pending.append(a)
                a += size
                continue
            if not self.is_code_addr(tgt) or not self.img.readable(tgt, 2):
                break
            if decode(self.img.u16(tgt), tgt).kind == INVALID:
                break
            empty.extend(pending)
            pending.clear()
            entries.append((a, tgt))
            # An offset table cannot run past the first thing it points at.
            if load != "long" and base > ins.addr:
                limit = tgt if limit is None else min(limit, tgt)
            a += size
        if not entries:
            return None
        for entry_addr in [e for e, _ in entries] + empty:
            self._mark_data(entry_addr, {"long": "long", "word": "short",
                                         "byte": "byte"}[load], size)
        return Table(dispatch=ins.addr, base=base, kind=load, entries=entries)

    # -- pointer tables in data ----------------------------------------
    def looks_like_code(self, addr, max_insns=64, min_insns=4):
        """Conservative test that `addr` starts a real instruction sequence.

        Decoding must reach a terminator without hitting an invalid encoding or
        a branch that leaves mapped memory. Both guards matter: constant fill
        such as 0xAAAA decodes as a lone `bra` to nowhere, which would otherwise
        pass instantly on the very first instruction.
        """
        if addr & 1 or not self.is_code_addr(addr):
            return False
        a = addr
        for i in range(max_insns):
            if not self.img.readable(a, 2):
                return False
            ins = decode(self.img.u16(a), a)
            if ins.kind == INVALID:
                return False
            if ins.target is not None and not self.is_code_addr(ins.target):
                return False
            if ins.kind in (RET, JUMP, JUMP_IND):
                return i + 1 >= min_insns
            a += 2
        return False

    # Encodings a walk can reach that say the bytes are not code after all.
    # Both are things no assembler emits and this SH-2 cannot execute, which is
    # what makes them usable as a veto rather than a heuristic.
    def _sweep_rejects(self, addr, max_insns=64):
        """`trapa`, or a control transfer in a delayed transfer's delay slot."""
        a = addr
        for _ in range(max_insns):
            if not self.img.readable(a, 2):
                return False
            ins = decode(self.img.u16(a), a)
            if ins.mnem == "trapa":
                return True
            if ins.kind in (RET, JUMP, JUMP_IND, BRANCH, CALL, CALL_IND):
                if not ins.delay or not self.img.readable(a + 2, 2):
                    return False
                slot = decode(self.img.u16(a + 2), a + 2)
                # A branch cannot sit in a delay slot. 0x06005BA4 is `bra`
                # followed by `bra`, which is a longword pointer table read as
                # instructions — the one false positive the two-instruction
                # bound let through.
                return slot.kind in (RET, JUMP, JUMP_IND, BRANCH, CALL, CALL_IND)
            if ins.kind == INVALID:
                return False
            a += 2
        return False

    def scan_self_relative(self, lo, hi, min_insns=2):
        """Seed the targets of a script stream of 16-bit self-relative offsets.

        `0x06005E9C` is a byte-code interpreter for the 32X's 3D objects:

            06005E9E  mov.w @r12+,r1
            06005EA0  add   r12,r1
            06005EA4  jmp   @r1

        so an entry is an offset from the address just past itself, and the
        stream it walks is a *runtime* pointer — `mov.l @(44,r13),r12`, a field
        of an object. Nothing static can say where a given stream is, which is
        why 5.6 KB of the master's image was reached by no branch, no table and
        no sweep, and why the special stage killed the master on a transfer to
        0x06006188 with no block.

        What is static is the arithmetic. Applied to every word that is not
        already an instruction, and keeping only the targets that decode as a
        real sequence, it recovers the handlers without having to find the
        streams: a script entry and its handler are in the same region by
        construction, because the offset is only sixteen bits.

        A handler can also be two instructions long, and `min_insns` is 2 here
        for the same reason the installed-handler sweep on the 68000 side had
        to stop inheriting four: that bound exists to keep a *blind* sweep
        honest, and this is not a blind sweep — the target is named by the
        interpreter's own arithmetic. `0x06006D50` is `mova` then `rts`, and
        four turned it away.
        """
        found = 0
        for a in range(lo, hi, 2):
            if a in self.code or not self.img.readable(a, 2):
                continue                      # an instruction, not an offset
            t = a + 2 + self.img.s16(a)
            if not (lo <= t < hi) or t in self.code:
                continue
            if not self.looks_like_code(t, min_insns=min_insns):
                continue
            # `looks_like_code` treats `trapa` as a terminator, which is what
            # lets a walk that has run into a literal pool finish and be
            # believed. Existing seeds depend on that — 0x06004A24 is accepted
            # only because of it — so the veto belongs here rather than there.
            if self._sweep_rejects(t):
                continue
            self.add_function(t, "script dispatch target")
            found += 1
        return found

    def scan_pointer_tables(self, lo, hi, min_run=2, min_insns=1):
        """Seed functions from arrays of code pointers sitting in data.

        Handler tables reached as `mov.l @(r0,rX),rY` where rX came from memory
        are invisible to the mova-based idioms, but they are still recognisable
        by shape: a run of aligned words that each point at plausible code.
        Requiring a run of at least `min_run` keeps stray constants out.

        A null slot does not end the run. A table whose nth case is "there is
        no handler" writes it as a zero, and ending there threw away everything
        after it: the table at 0x06000560 has four pointers and a zero fourth,
        and 0x06000584 — the fifth — is what the special stage transfers to and
        what killed the master with no block. The 68000 side learned the same
        thing from a table whose empty case is `nop / rts`. A zero contributes
        nothing to the run's length, so a lone zero between two constants still
        cannot pass for a table.
        """
        added = 0
        run = []
        holes = []

        def flush():
            nonlocal added
            if len(run) >= min_run:
                for a, v in run:
                    self._mark_data(a, "long", 4, firm=False)
                    if v not in self.func_entries and self.add_function(v, "ptr table"):
                        added += 1
                for a in holes:
                    self._mark_data(a, "long", 4, firm=False)
            run.clear()
            holes.clear()

        a = lo + (-lo % 4)
        while a + 4 <= hi:
            if a in self.code or a in self.data:
                flush()
                a += 4
                continue
            v = self.img.u32(a)
            # A run of consecutive valid pointers is itself strong evidence, so
            # short handlers (a bare `rts` stub) are acceptable candidates here.
            if self.looks_like_code(v, min_insns=min_insns):
                run.append((a, v))
            elif v == 0 and run:
                holes.append(a)          # an empty case, not the end of the table
            else:
                flush()
            a += 4
        flush()
        return added

    def scan_after_returns(self):
        """Seed functions that begin immediately after a run of code ends.

        Handlers only ever reached through a register callback have no visible
        xref, but the compiler still lays them out end to end, so the address
        after an `rts` is a strong candidate. So is the address after an
        unconditional `bra` or `jmp`: control cannot fall through either of
        those, so whatever follows is reachable only indirectly — which is
        exactly the case a static cross-reference search cannot see. A
        conditional branch is not in this class; its next address falls through
        and is covered already.

        A routine reached only through a pointer is also aligned, so what
        follows is often one or two `nop`s of padding and *then* the entry
        point. Seeding only the padding leaves the real entry mid-block, where
        an indirect transfer to it has nothing to dispatch to — which is how
        0x06004A24 went missing while every byte of it was translated.
        """
        added = 0
        for end, (kind, _) in list(self.block_ends.items()):
            if kind not in (RET, JUMP, JUMP_IND) or end in self.code \
                    or end in self.data:
                continue
            if not self.looks_like_code(end):
                continue
            if self.add_function(end, "after " + kind):
                added += 1
            a = end
            while self.img.readable(a, 2) and self.img.u16(a) == 0x0009:
                a += 2
                if self.looks_like_code(a) and self.add_function(a, "after padding"):
                    added += 1
        return added

    # -- phase 2: blocks and functions ---------------------------------
    def _build_functions(self):
        """Cut the traced instruction stream into blocks at the recorded leaders."""
        self.blocks = {}
        ordered = sorted(self.code)
        cur = None
        for addr in ordered:
            contiguous = cur is not None and addr == cur.end
            if cur is not None and (not contiguous or addr in self.leaders):
                if contiguous:
                    cur.succs.add(addr)      # ran straight into a branch target
                cur = None
            if cur is None:
                cur = Block(start=addr)
                self.blocks[addr] = cur
            ins = self.insns[addr]
            cur.insns.append(ins)
            cur.end = addr + 2
            # `block_ends` is keyed by end address, so a delayed branch closes
            # its block only after the delay slot has been added, while the
            # successor edges still come from the branch itself.
            if cur.end in self.block_ends:
                kind, target = self.block_ends[cur.end]
                if kind == BRANCH:
                    cur.succs |= {target, cur.end}
                elif kind == JUMP:
                    cur.succs.add(target)
                elif kind in (CALL, CALL_IND):
                    # Not a jump edge, but the return point belongs to this
                    # function and must be grouped with it.
                    cur.succs.add(cur.end)
                cur = None

        self.funcs = {}
        for entry in sorted(self.func_entries):
            if entry not in self.blocks:
                continue
            fn = Function(start=entry)
            seen = set()
            stack = [entry]
            while stack:
                b = stack.pop()
                if b in seen or b not in self.blocks:
                    continue
                # Do not absorb another function's body.
                if b != entry and b in self.func_entries:
                    continue
                seen.add(b)
                blk = self.blocks[b]
                fn.blocks[b] = blk
                stack.extend(blk.succs)
            self.funcs[entry] = fn
        for addr, kind in self.indirect:
            owner = self._owner(addr)
            if owner:
                self.funcs[owner].unresolved.append((addr, kind))
        return self.funcs

    def _owner(self, addr):
        best = None
        for e, fn in self.funcs.items():
            for b in fn.blocks.values():
                if b.start <= addr < b.end:
                    return e
        return best

    # ------------------------------------------------------------------
    def stats(self):
        return {
            "functions": len(getattr(self, "funcs", {})),
            "blocks": len(getattr(self, "blocks", {})),
            "code_insns": len(self.code),
            "code_bytes": len(self.code) * 2,
            "data_bytes": len(self.data),
            "tables": len(self.tables),
            "unresolved": len(self.indirect),
        }
