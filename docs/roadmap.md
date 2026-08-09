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

**M3 — SH-2 → C recompiler**

Emit one C function per discovered function, registers in a context struct,
memory through inlined accessors. Delay slots and the `T` bit are the two things
that must be modelled exactly. Start with the SH-2 because it is small and its
correctness is easy to check against an emulator trace.

**M4 — runtime skeleton**

Memory map, 32X VDP framebuffer, palette, SDL window and input. Enough to see
output. Boot the recompiled SH-2 against an interpreted 68000 (Musashi) so there
is a running system long before the 68000 recompiler exists.

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
