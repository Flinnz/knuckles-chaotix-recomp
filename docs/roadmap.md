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
| Genesis VDP | native: planes, sprites, scroll, DMA ✅ | moderate |
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

**M5 — first pixels** 🔵 *a picture, from the Mega Drive half*

There is a frame. It is a recognisable Knuckles' Chaotix level — sky, palm
trees, machinery, plant sprites, foreground platforms — and it took one thing
that had been missing all along rather than another CPU bug.

Asking where the pixels were supposed to come from answered it. At 3,000 frames
the 32X frame buffer holds a line table and no pixels, while the Mega Drive side
holds 45 KB of tiles, a full 64-colour palette, both name tables nearly full,
and the display enabled. **In this game the first picture is a Mega Drive
picture**, and `src/gen68k.c` was keeping all of that as state without anything
turning it into pixels.

`src/genvdp.c` now does: planes A and B with per-line horizontal and
whole-screen vertical scroll, sprites in link order with the size and flip
fields, the six-way priority ordering between planes and sprites, and the
three-bits-a-channel palette. The 32X bitmap composites over the result, index 0
transparent. Register decoding is general, but only what this game selects is
exercised — H40, two 64x32 planes — and the window plane, shadow/highlight,
interlace and the per-line sprite limits are not modelled.

Getting there needed three smaller things too. The reference showed writes into
0x000000-0x0000FF and a `jsr` into it, so that region is adapter RAM rather than
cartridge ROM. Cache purges — writes to the SH7604's associative purge area at
0x40000000 — were being counted as unmapped. And the 32X has two frame buffers
selected by FBCR bit 0, where we had one, so drawing and display were the same
memory.

**What is still missing is the 32X image.** The SH-2 runs the right code — its
hottest function is the bit reader at 0x06000856, 80,082 entries, the same
decompressor the reference sits in — but nothing reaches the frame buffer, and
the line table it does write covers 128 lines of the 224 on screen. Where an
entry is zero the runtime draws nothing; hardware would scan out the line
table's own bytes as pixel indices, which is exactly the rainbow stripes that
first showed up over the picture. `--layers` switches each layer off
independently, which is how that was pinned down.

### The SH-2 gets the same treatment

`tools/diffsh2.py` now does for the SH-2s what `diff68k.py` does for the 68000,
and the alignment they share — the monotone forward search, the classification
into trips/state/flow, the reporting — moved into `tools/tracediff.py`. The
68000's output is byte-identical across that refactor, which is what says the
move was a move and not a rewrite.

**Block granularity, but not from the entry ring.** The plan was to compare the
existing ring against the reference filtered to those addresses. Reading the
generated code settled it differently: the ring records *dispatch* entries —
calls, returns, tail transfers — because that is what passes through the
trampoline, while an intra-function branch is a `goto` and a fall-through is
nothing at all. Matching the reference to that needs a static, delay-slot-aware
model of which transfers route through the runtime, which is more machinery than
the alternative and less exact.

One hook at each basic block label is two lines of codegen and makes the two
streams the same sequence by construction: the reference logs every instruction,
so filtered to the block addresses it *is* our stream. `SH2_BLOCK` writes the
whole register state, so this is not merely "did we enter this function" but the
full state on entry — and because a basic block always executes whole, our
instruction counts between two entries are exact too, which is what makes a trip
count comparable against the reference's. It costs a load and a not-taken branch
per block: 200 headless frames go from 0.19s to 0.52s, on a workload that is
almost entirely four-instruction idle-poll blocks and so close to the worst case
for a per-block hook.

**The recompiled code is not the problem.** Over the whole extracted boot, for
both SH-2s, *every instruction the reference ran is inside a block we have* —
discovery missed nothing it reaches. The master's control flow then matches the
reference exactly from its cartridge entry to the end of our trace: no flow
divergence at all.

**The BIOS hands over a register state, and we were starting from zero.** The
first thing the diff found was that nothing matched — 0 blocks of 141 agreed,
because the reference arrives at `0x060001A0` carrying what the adapter's boot
ROM left behind. `src/mars_main.c` now supplies it, in the same vein as the
checksum and the comm words. Three parts of it are load-bearing rather than
cosmetic: `gbr = 0x20004000`, the 32X system register block, which the cartridge
addresses GBR-relative from `0x06004770` on and only ever loads from `r14` — so
zero sent all of it to address 0; `sr = 0x000000F1`; and the slave's stack
pointer, which the cartridge's own vector table gives as `0xC0000800` inside the
cache data array, where the reference shows the slave entering with `0x0603F800`
in SDRAM. The adapter does not take the slave's stack from the cartridge table.
Agreement went from 0 to 128 of 141 blocks, and the master now gets far enough
to upload the 32X palette — 471 of 512 bytes, where it had never been written.

**What is left on the master is scheduling, not semantics.** The one remaining
state divergence in the boot is the frame-select bit: our master's init runs to
completion before the 68000 starts, and the 68000's own 32X init at
`0x880738`-`0x880750` then toggles FS underneath it. On hardware those overlap —
the 68000 does that while the master is still in the BIOS summing the cartridge.
The `bclr`/`bset` loops there run one iteration in the reference and two in ours,
which is the same fact seen from the other CPU.

**The slave never receives its interrupt.** It diverges after 103 reference
lines and does not rejoin: from its idle delay loop at `0x060002FC` the reference
vectors away, runs the cache-array routine at `0xC0000004`, and comes back. Ours
spins in the delay loop forever, because nothing delivers the 32X command
interrupt to recompiled code.

**The draw path, mined from the pre-reset segments.** Command 2 reaches
`0x0600097C`; `0x060009A6` is not a handler but a command-*list* interpreter —
`mov.b @r14+,r1`, then `braf` through a table of 16-bit offsets at `0x060009B4`,
each entry returning to the fetch. Opcode 1 is `0x06000A50`, which runs on to the
stub at `0x06000A88` whose `bsrf` enters `0x06001F18` — the routine holding the
frame buffer write at `0x060021EA`. The reference's `pr = 0x06000A8E` at that
write confirms the call site exactly.

**So there is no 32X picture because the command's payload never arrives.** We
*do* receive command 2 — thirteen times in 200 frames — and run its handler. The
list it is supposed to interpret comes over the **DREQ FIFO at `0xA15112`**,
which the runtime discards: `mars_reg_write` handles the DREQ control register
and swallows the rest of the block. The 68000 sets the transfer up at
`0x8819AE`-`0x8819E0` — length to `0xA15110`, source to `0xA15107`, raise the
master's command interrupt at `0xA15103`, wait for the SH-2 to clear it, then
push words into the FIFO. Our master reads a stale list at `0x06004xxx`, gets
opcode 0 instead of opcode 1, and returns without drawing.

That is the pivot condition, half met and more specific than it was written: not
"we never receive the command", but "the command arrives empty".

### The 32X draws

The missing piece was one path, and building it turned out to be three things
that are really the same thing: the SH-2s had no way to be entered except by the
scheduler, so nothing that the 68000 asks of them mid-frame could happen.

**Interrupt delivery.** `mars_deliver_int` takes an external interrupt and runs
the handler to completion. The SH-2 takes these through auto-vectors 64-71, two
levels to a vector, so the command interrupt at level 8 lands on vector 68 —
and both of this cartridge's tables fill all eight slots with one dispatcher,
0x060001B0 on the master and 0x060001F8 on the slave, which reads the accepted
level back out of SR to pick the real handler. Nothing is pushed on the SH-2
stack: `rte` already returns to the runtime rather than popping a frame.

**The DREQ FIFO.** The master's command handler at 0x06001334 arms DMAC channel
0 with SAR0 = 0x20004012 — the FIFO read port, address fixed — DAR0 = the list
buffer in SDRAM, and TCR0 = the word count it reads back from 0x20004010. The
channel is therefore always armed *before* the 68000 pushes anything, so
draining on push is exact rather than a simplification, and no general DMAC is
needed.

**The command rendezvous is synchronous now.** Commands used to be collected
during the frame and replayed at the end, through a queue that intercepted comm
0 — because a command written a whole frame early would have been wiped by the
zero the SH-2's dispatch loop writes on entry. With the SH-2 reachable from a
68000 register write, the 68000 simply runs it at the moment it posts, entering
at the poll rather than the loop head so the command it just wrote survives. The
queue is gone, and with it the reason the 68000 could never see its
acknowledgement: it waits for comm 0 to read back zero, at 0x8845CE among other
places, and that only happens if the SH-2 has actually run.

**There is a 32X picture.** The frame buffer goes from nothing beyond the line
table to 40,496 bytes of pixels, and `--layers 8` shows what they are: two
panels of animated cloud, drawn by the decompressor the master had been running
against an empty command list all along. 5,876 words now cross the FIFO in 200
frames. The 68000's boot still matches the reference exactly, and all eight
recompiler semantics tests still pass.

**Where it stops now** is 0x8845D4, waiting on that same comm 0, because the
350th command transfers to **0x06004A24** and there is no block there. The
address is real code — it sits after a `bra`, its delay slot and an alignment
`nop`, so nothing falls through to it and no static xref reaches it; it is one
of the unresolved indirect transfers M1 left open.

**And the experiment to find it turned up a bigger bug.** Seeding discovery
after an unconditional `bra`/`jmp` the way it is already seeded after `rts` —
control cannot fall through either, so what follows is reachable only
indirectly — finds 0x06004A24, takes the front end from 208 to 247 functions,
and keeps both images' byte-exact round-trips. It also breaks the master's boot,
and the SH-2 diff said why in one run: at the end of init the reference goes to
0x060008F2 and we went to 0x06001250, with *identical registers*.

The cause is not the new seeding. **Falling out of a block was textual
adjacency**: a block that did not end in an unconditional transfer simply ran
into whichever label the emitter had put next. That is only correct when a
function's blocks are contiguous in memory, and 288 of them are not — a function
that shares a tail with another owns two disjoint runs, so the fall-through at
the end of the first run continued into the second.

Every block now ends in an explicit `goto` or `return`. That also makes the two
owners of a shared block interchangeable, so `sh2_functions[]` holds one row per
address instead of one per (block, owner) pair — it had duplicate keys for the
runtime's binary search to pick between arbitrarily, which had been true all
along and only became reachable when the new seeding found a case where the two
owners disagreed.

With that settled the discovery change goes in: **208 -> 247 functions**, both
images still reassemble byte for byte, both boots still match the reference with
no fatal divergence, and the run no longer stops at frame 380.

### What the picture is waiting on now

Two thirds of the objects the master draws have a valid asset pointer and one
third is null, which is what the 10.8 million reads of address 0 are. The
pointers live in a two-level table at `0x06003614` that starts as all-ones and
is filled one slot at a time by **command 7** (`0x060010D8`): read a slot index
and an asset id from the command payload, decompress from the cartridge index at
`0x020A0000` into the next free SDRAM, store the destination in the slot, and
post the new free pointer back to comm 3. Our 68000 issues that command once in
400 frames, so most slots are never filled. Why it issues one and not many is
the next thread, and it is a 68000-side question rather than an SH-2 one.

The 32X image also degrades after frame ~350: the line table becomes 256 entries
all holding the same offset, so every scanline scans out the same row and the
screen goes flat. At 200 frames it is still a real picture.

### Carried debts

* **No regression gate on the runtime.** The front end has byte-exact
  round-trips and the recompiler has semantics tests, but nothing notices if the
  boot diff stops matching. `tools/diff68k.py --ref-lines 20213` is one command
  and already exits non-zero on a fatal divergence; `tools/diffsh2.py --cpu
  master` is now a second one.
* **The asset table is barely filled.** Command 7 loads one slot per call and
  our 68000 issues it once; a third of the objects drawn therefore dereference
  a null pointer.
* **Only the command interrupt is delivered.** The slave takes 2,197 PWM
  interrupts in one reference segment and we raise none, so it still idles in
  the delay loop the diff caught it in — `tools/diffsh2.py --cpu slave` goes
  fatal at reference line 103 and will keep doing so until it has a timer to
  drive that interrupt from.
* **Genesis DMA fill and copy are skipped.** The reference issues 65,535 fills
  during boot; `vdp_dma` returns early on both.
* **The 32X frame-select polarity is a guess.** Displayed is taken to be
  `fb[FS]` and the CPUs get the other one; nothing has confirmed which way round
  it is.
* **The frame has not been checked against a real screenshot.** It is a
  plausible Chaotix level, which is not the same as a verified match.
* **Genesis VDP gaps:** no window plane, shadow/highlight, interlace, or the
  per-line sprite and pixel limits.

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
