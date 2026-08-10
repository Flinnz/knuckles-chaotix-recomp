# Roadmap

## The two things "decomp" can mean

**Matching decompilation** (the sonic-disasm / sm64 model) — hand-write C that a
period-correct compiler turns back into byte-identical ROM. The output is source,
not a program you run natively. For a 3 MB two-architecture title this is a
multi-year effort, and the 68000 half has no usable matching compiler story.

**Static recompilation** (the N64Recomp model) — mechanically translate the
original machine code into C, compile that with clang for arm64, and supply a
native runtime for everything that was hardware. The output is a native ARM
binary. This is what "recompile it to ARM" describes, and it is the plan below.

The two share their entire front end: decoder, code discovery, control-flow
recovery. Nothing invested in the disassembly is wasted if the goal later
shifts, so the front end is being built first regardless.

## What a native port actually has to replace

A 32X game is not one CPU. The port has to account for all of it:

| Original | Native replacement | Difficulty |
|---|---|---|
| 68000 game engine | recompiled C, or interpreter first | **the bulk of the work** |
| SH-2 master + slave | recompiled C (only 36 KB of code) | moderate |
| Z80 sound driver | interpret — it is tiny and self-contained | easy |
| 32X VDP | native: framebuffer, palette, line table, autofill | easy |
| Genesis VDP | native: planes, sprites, scroll, DMA | moderate |
| YM2612 + PSG + PWM | existing emulator cores (Nuked-OPN2 etc.) | easy |
| Controllers | SDL input | easy |

Realistically this is a purpose-built high-level 32X runtime with the CPU cores
replaced by recompiled native code. That framing matters: expect to write a
fair amount of hardware emulation regardless of how good the recompiler gets.

## Milestones

**M0 — foundation** ✅ *done*

ROM verified and mapped, MARS header decoded, SH-2 decoder validated exhaustively
against `sh-elf-objdump` (all 53,752 valid encodings agree), recursive-descent
discovery with literal-pool and jump-table recovery, 193 SH-2 functions found.

**M1 — finish the SH-2 front end** ✅ *done*

Two-phase CFG construction (leaders, then blocks) so backward branches split
runs instead of duplicating them; all three dispatch idioms recovered; the
cache-array overlay found and disassembled; function-pointer tables in data
swept; and a reassemblable listing emitted.

*Gate met:* `python3 tools/emit_asm.py --verify` reassembles the listing with
`sh-elf-as` to bytes identical to the cartridge — 36,864 + 1,024 bytes, on both
the JU and E images.

208 functions, 1,772 blocks, 15 dispatch tables. 8 indirect transfers remain
unresolved; all are runtime function pointers whose callee the caller picks, so
they need interprocedural dataflow rather than better pattern matching. Of the
unclassified remainder, ~190 bytes still look like code.

**M2 — 68000 front end** ✅ *done*

Decoder validated exhaustively against `m68k-elf-objdump` (100% of 45,496
comparable opcodes), discovery seeded from the exception vector table, and the
PC-relative indexed jump table — the engine's dispatch idiom — recovered.

*Gate met:* `python3 tools/disasm68k.py emit --verify` reassembles the **entire
3,145,728-byte cartridge** byte for byte, on both the JU and E images.

368 functions, 2,957 blocks, 11,655 instructions. 42 indirect transfers remain
unresolved. Code found so far is 1.3% of the ROM, which is expected at this
stage: most of the cartridge is compressed art, and the engine is reached
through data-driven tables that the next pass needs to follow.

**M3 — SH-2 → C recompiler** 🔵 *core done, coverage pending*

All 208 discovered SH-2 functions translate to C with no unhandled constructs,
and the result compiles clean for arm64 under `-Wall`. Semantics are checked
end to end by `tools/test_recomp.py`: an SH-2 test program is assembled,
recompiled, built for this machine, executed, and its answers compared against
values derived independently of any SH-2 model. All 8 cases pass, covering the
divide step, delay-slot ordering, carry propagation and shift kinds.

Still open before this milestone closes:

* **14 escaping branches.** A branch whose target lands in a block owned by
  another function falls back to indirect dispatch. Correct, but it defeats
  inlining and hides a real control-flow edge.
* **The PR model.** Calls become native calls and `rts` becomes `return`, with
  PR still set to the true return address so save/restore round-trips. Code
  that *computes* a return address to skip inline data is outside this model.
  No such site is known in this game, but nothing detects one yet.
* **Literal pool loads go through memory.** Correct, and leaves the constants
  patchable, but they are the obvious first optimisation.

**M4 — runtime skeleton** 🔵 *SH-2 half running, 68000 half absent*

`make run` boots the recompiled master SH-2 against a real 32X memory map:
SDRAM, cartridge, framebuffer with its overwrite image, palette, the 32X system
and VDP registers, the cache-array overlay, and the SH-2 on-chip peripheral
block. Zero unmapped accesses and zero missing call targets on a full boot.

The master runs its init and reaches its command dispatch loop, where it
selects packed-pixel mode and writes the framebuffer line table: 256 entries,
512-byte stride, line 0 starting immediately after the table. That is exactly
what correct 32X video initialisation produces, and it is checkable rather than
merely non-empty.

**There is no picture yet, and that is the honest state.** In this game the
68000 is the engine — it decides what to draw and posts commands to the SH-2s.
With no 68000, the SH-2 initialises video and then waits, so the framebuffer
holds a line table and nothing else, and the palette is never uploaded.

Two stand-ins keep the SH-2 moving where it would rendezvous with the 68000:
the "REDY" handshake word in SDRAM, and a scripted command queue on comm
register 0. Polling loops that a real machine would exit are handled by a
free-running counter behind the VDP status bits, plus a watchdog that unwinds
instead of hanging.

The 68000 is now present: Musashi (MIT) interprets it, `src/m68kconf.h` trims
it to a bare 68000, and `src/gen68k.c` supplies its address space — cartridge
windows, work RAM, I/O, a Genesis VDP modelled far enough not to stall, and the
32X registers, which are shared with the SH-2 view so there is one
implementation of the 32X VDP and its status bits.

The 68000 boots from the cartridge reset vector, passes the `"MARS"` signature
and adapter-enable checks, and takes control of the 32X VDP. Routing the 32X
VDP registers to both CPUs took unmapped accesses from ~756,000 per 60 frames
to 9 across 600 frames.

The slave SH-2 now runs too. It needs its own vector table at 0x06000080, and
its stack pointer comes out as 0xC0000800 — inside the **cache data array**,
not SDRAM, which fits the routine the slave copies there at boot.

**Where it stops:** at 0x8809A6 the 68000 waits for `"M_OK"` in comm 0-1 and
`"S_OK"` in comm 2-3. Neither is posted: the functions that write them
(0x06001250 and 0x06000308) are not reached by either init path, so the 68000
still spins at 0x88099E and comm 0-1 holds only the 0xFFFF the master's boot
writes.

**Tail transfers are now modelled properly.** A generated function returns the
address of a `jmp`, or of a branch that leaves the function, and `sh2_call`
loops on that; only genuine `bsr`/`jsr` calls nest. A hardware dispatch loop is
therefore an iteration rather than a recursion, and the segfault is gone. This
also subsumes the 14 escaping branches previously noted under M3 — they are
tail transfers, not indirect dispatches. All 8 recompiler semantics tests still
pass under the new calling convention.

**It did not unblock the handshake**, and the trace hook (`--trace`) then
found why in one run. Because every function entry passes through the dispatch
loop, tail transfers included, the trace needed no instrumentation in the
generated code at all.

The slave's idle handler at 0x060002F6 burns a short delay loop and then does
`bt 0x060002D4` — branching back to the dispatch loop head **instead of
executing `rts`**. It is reached by `bsrf` from that same loop, so every poll
iteration enters a handler that never returns.

That is ordinary SH-2 practice and costs nothing on hardware: `bsr`/`jsr` only
write PR, they do not push a stack frame, so abandoning the return address is
free. The recompiler models a call as a *nested C call*, which does create a
frame, and it never unwinds — 256 iterations deep and the bound trips.

**Calls now work the way the hardware does.** The SH-2 has no call stack, only
PR, so `bsr`/`jsr` set PR and transfer, and `rts` transfers to PR. Every control
transfer returns its destination to one flat trampoline loop; nothing recurses.

Two consequences had to be handled: a return address is mid-function, so the
instruction after a call now starts a basic block (1,772 -> 1,920 blocks, with
the whole-image round-trip still byte-exact), and a function can be entered at
any of its blocks through an `entry` parameter, so the dispatch table is keyed
by block rather than by function.

The recursion is gone — the depth bound now trips zero times, and it is
unnecessary rather than merely tolerated. All 8 semantics tests pass under the
new calling convention.

It also caught a latent bug in the test program itself, which made a call and
then returned without ever preserving PR. The old nested-call model masked it;
the faithful one spun. Real SH-2 code always saves PR around a call, and the
test now does too.

**The slave is no longer misbehaving.** It idles on its handler at 0x060002F6 —
burn a delay loop, return to the dispatch head, read a command byte, repeat —
which is what a slave with nothing to do is supposed to do. The master gets
substantially further into init than before.

A reference trace of the JU ROM then settled two questions at once. The logs
carry every CPU — SHM, SHS, the 68000 and the Z80 — one line per instruction
with full register state before it executes, which is exactly the oracle this
project needed.

**The ready words come from the 32X BIOS, not from the cartridge.** `M_OK` is
written by the slave executing at 0x000001C0, inside the adapter's own boot ROM,
so the cartridge function at 0x06001250 that also holds that literal is a red
herring and never runs. We have no BIOS image and start the SH-2s at the
cartridge entry points, so the handshake is now supplied directly by the runtime
— a high-level stand-in for BIOS code we do not have.

**Our 68000 diverges before it ever reaches the handshake.** The reference run
never executes 0x88099E, the loop ours spins in. Its path is
0x8807E2 `movem.l (a6),...` -> 0x880800 `bcc` -> 0x88081E -> `tst.w d0` ->
0x880824 `bpl 0x8809A6`. Ours takes a different branch somewhere upstream and
lands in code the real machine never runs.

**The comparison is now mechanical.** `build/mars --trace68k FILE` logs one line
per 68000 instruction — PC, opcode word and the register state before it
executes — and `tools/diff68k.py` aligns that against the reference, finds the
reset in the logs itself, and reports every difference with both register sets.

Aligning the two took one insight. The reference tracer does not log a complete
stream: it collapses repeats and reports each run as `[Omitted: N]`. Mirroring
that policy is the wrong move, because our trip counts legitimately differ from
the reference's — it spins 8,277 times on a VDP busy bit our model clears
immediately, so any prediction built on those counts lands 16,000 instructions
into unrelated code. Instead our side logs everything, alignment is a monotone
forward search, and the counts are used for *diagnosis*: comparing what we ran
between two reference lines against what they ran turns a differing trip count
into a finding rather than a lost alignment.

**18,614 of the reference's 20,178 boot instructions now agree in lock step**,
and the two bugs in between are found rather than guessed at.

The first was mundane and had been hiding since the 68000 was added:
`src/m68kconf.h` never took effect. Musashi's sources say `#include
"m68kconf.h"`, which a quoted include resolves to the copy next to them in
`third_party/`, so `-Isrc` never overrode anything. Forcing ours in first claims
the shared header guard and the vendored file expands to nothing.

The second was the divergence itself, at **0x880790**: `move.b ($a15180),d2`
reads the 32X bitmap mode register, and the security check at 0x880798 refuses
to boot unless bit 15 is set. We returned 0, so the 68000 fell into the failure
path — which is exactly how it ended up spinning at 0x88099E. Bit 15 reads back
set whatever is written: the boot writes 0x0000 there at 0x880730, and a later
word read at 0x88196A still returns 0x8000. Open bus does not explain it, since
the word the 68000 had just prefetched at that point was 0x0040. The bit's
*meaning* is unidentified — it appears in no register description we have — but
its behaviour is pinned by two independent reads, and modelling it moved the
68000 eleven reference instructions further, onto the reference's own path.

**The 68000 now runs the entire boot in step with the reference.** Working down
the list the diff produced turned up three more gaps, each of the same kind: the
adapter does something for the cartridge that we were not doing for it.

*The cartridge checksum.* The 68000 reads the header's checksum word at
0x88018E — 0xB61C — and spins at 0x8807C2 until comm register 4 holds the same
value. The reference's master SH-2 posts it from 0x00000284, having summed the
cartridge in a loop at 0x00000278 that costs 9,435,642 instructions. That is
BIOS code, so the runtime supplies it, and the sum is computed rather than
asserted: 16-bit words from 0x200 to the end, which reproduces the header word
of both the JU and E images exactly.

*The handshake was being posted too early.* The 68000 zeroes comm 4 at 0x0003F6
and comm 0-3 at 0x8806F0, as part of its own boot, and only then waits for the
BIOS to fill them — so "M_OK"/"S_OK" written before the 68000 started were being
wiped, and would have hung at 0x8809A6 as soon as the checksum let us reach it.
Each value is now delivered once, the first time the 68000 is found to have
cleared it.

*The adapter supplies the vector table.* At 0x8809E0 the 68000 does
`jsr ($0000c0)`, and the reference finds four instructions there — RV on, a byte
to the mapper at 0xA130F1, RV off, `rts` — where the cartridge's own image has
the 0x00880B2E handler pointer that fills every reserved vector slot. It also
*writes* vector 28 at 0x000070, twice. So 0x000000-0x0000FF is 256 bytes of
adapter RAM holding the vectors plus BIOS helpers in the slots the 68000 leaves
reserved, not cartridge ROM; everything from 0x000100 up is the cartridge, which
is why the security stub at 0x0003F0 matched all along. `src/gen68k.c` seeds that
region from the cartridge's own table and assembles the one helper the cartridge
calls, its four opcodes checked against the reference's.

One more, found the same way: the 68000 raises the master SH-2's command
interrupt at 0x8819BE and waits at 0x8819C6 for the SH-2's handler to clear it.
Nothing clears it here, so the request is recorded as already acknowledged — a
stand-in, since the DREQ transfer it is arranging still needs an SH-2 at the
other end.

*Gate met:* `python3 tools/diff68k.py --ref-lines 20213` walks the whole boot,
20,062 of the reference's 20,178 instructions agreeing exactly and no divergence
in control flow. The 68000 now posts real commands to the SH-2 (18 in 20 frames,
13 serviced), drives Genesis VDP DMA, and there are zero unmapped accesses on
the SH-2 side — the last of those turned out to be cache management, writes to
the SH7604's associative purge area at 0x40000000, which are no-ops for a model
with no cache.

The 34 differences that remain in the boot are all understood and none changes a
branch. Polls that finish instantly for us where the reference waits: the VDP
DMA-busy bit (16,555 instructions against our 1), the autofill FEN bit, and the
comm rendezvous themselves, now that we answer them at once rather than after
336,174 instructions. Registers that differ without consequence: the version
register's hardware-revision nibble, and the undriven bits 14-8 of the bitmap
mode register. And one path the reference takes that we do not — its controller
ports are already initialised, so it skips the cold-boot shortcut at 0x000424 —
where both roads reconverge four instructions later at 0x000436.

**Past the boot the oracle thins out.** The reference extract now follows all
four remaining log segments, 54,647 lines in all, and our run reaches the end of
it without control flow ever parting for good. But only 22,037 of those 54,081
instructions agree exactly, because the first vblank interrupt lands at line
20,781 and there are 102 of them: we inject one per frame at a fixed point,
where the reference takes them on real timing. Comparing that section usefully
needs interrupt timing modelled, not just more trace.

Also outstanding: `jmp` is translated as call-then-return, so a hardware
dispatch loop that never returns becomes a finite call chain that unwinds. It
is why the master "returns" at all. Harmless here, wrong in general, and worth
fixing when the 68000 starts driving real work.

**M5 — first pixels**

Reach the Sega logo, then the title screen. This is where the hard bugs surface.

**M6 — sound, then the 68000 recompiler**

YM2612/PSG/PWM, then replace the 68000 interpreter with recompiled code and
optimise.

## Verification strategy

The thing that makes or breaks a project like this is having ground truth. The
plan is differential testing against a known-good emulator: run the same ROM in
an existing 32X emulator, log every CPU state transition and every hardware
access, and replay the same trace through the recompiled build. Divergence
pinpoints the exact instruction that broke. Building this early is cheaper than
debugging a black screen later.

## Honest scope note

M0–M4 is a realistic near-term arc. M5 onward is where comparable projects have
historically spent most of their time. Nothing here is blocked on unknowns — the
architecture is fully documented and the code is all statically reachable — but
it is a long project, and the 68000 engine is the bulk of it.
