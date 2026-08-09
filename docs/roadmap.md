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

**The fix is to model calls the way the hardware does.** SH-2 has no call
stack, only PR, so the flat trampoline should cover calls too:

* `bsr`/`jsr target` -> `c->pr = ret; return target;`
* `rts` -> `return c->pr;`

Everything becomes one loop with no C recursion at all, and code that saves and
restores PR around a call keeps working because PR is just a context field.
This is the M3 "PR model" caveat, now confirmed as hit in practice rather than
hypothetical, and it subsumes the depth bound entirely.

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
