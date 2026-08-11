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

*Later:* 561 functions, 7,943 blocks, 24,578 instructions, 2.8% of the ROM —
see "The 68000 front end had never been asked what it was missing" below. Both
images still reassemble byte for byte.

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

**M4 — runtime skeleton** ✅ *both CPUs running, in step with the reference*

`make run` boots the recompiled master SH-2 against a real 32X memory map:
SDRAM, cartridge, framebuffer with its overwrite image, palette, the 32X system
and VDP registers, the cache-array overlay, and the SH-2 on-chip peripheral
block. Zero unmapped accesses and zero missing call targets on a full boot.

The master runs its init and reaches its command dispatch loop, where it
selects packed-pixel mode and writes the framebuffer line table: 256 entries,
512-byte stride, line 0 starting immediately after the table. That is exactly
what correct 32X video initialisation produces, and it is checkable rather than
merely non-empty.

*From here each milestone reads in order, and later paragraphs supersede earlier
ones — the headings say where it ended up.*

**There was no picture at that point, and that was the honest state.** In this
game the 68000 is the engine — it decides what to draw and posts commands to the
SH-2s. With no 68000, the SH-2 initialised video and then waited, so the frame
buffer held a line table and nothing else, and the palette was never uploaded.

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

**M5 — first pixels** 🔵 *both halves draw; the 32X image is the SH-2's own*

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

### The 32X image is the SH-2's own

Separating the layers settles what draws what. At 200 frames `--layers 8` — the
32X buffer alone — is **byte-identical to the full composite**, and `--layers 7`
— the Mega Drive alone — is black, with 0 of 65,536 VRAM bytes and 0 of 64 CRAM
entries set. Every pixel on screen at that point came out of the recompiled
SH-2. That is the reverse of the first frame this project produced, which was a
Mega Drive picture with an empty 32X buffer composited over it.

The chain behind it, in block entries over 200 frames: the 68000 posts command 2
and streams the display list through the DREQ FIFO, the master's interrupt
handler DMAs it into SDRAM, the list interpreter at `0x060009A6` dispatches
opcode 1, and `0x06001F18` decompresses cartridge art and writes packed pixels —
`0x06000856`, the decompressor's bit reader, 80,082 entries, and `0x060021EA`,
the frame buffer write itself, **1,254,400** of them.

### What the picture is waiting on now

Two thirds of the objects the master draws have a valid asset pointer and one
third is null, which is what the 10.8 million reads of address 0 are. The
pointers live in a two-level table at `0x06003614` that starts as all-ones and
is filled one slot at a time by **command 7** (`0x060010D8`): read a slot index
and an asset id from the command payload, decompress from the cartridge index at
`0x020A0000` into the next free SDRAM, store the destination in the slot, and
post the new free pointer back to comm 3. Our 68000 issues that command once in
400 frames, so most slots are never filled.

The 32X image also degrades after frame ~350: the line table becomes 256 entries
all holding the same offset, so every scanline scans out the same row and the
screen goes flat. At 200 frames it is still a real picture.

### The slave gets its clock

The PWM interrupt is the slave's only clock, and its rate is the machine's own
rather than a number picked to look right. The sound driver programs the two
registers from `0xC0000008` — control `0x0105`, so TM is 1 and the interrupt
comes once per PWM cycle, and cycle `0x417` — and then idles. SH-2 clock over
cycle over TM is 21,957 Hz, or **366 interrupts to a 60 Hz frame**. The
reference logs 4,877 of them against 13 of the 68000's, or 375 apiece: the same
number arrived at from the other end.

The frame is now run in slices with one interrupt between each, rather than as
one block with the interrupts bunched at the end. What that buys is not audio
timing — nothing consumes the samples yet — but that the two CPUs interleave the
way the reference shows them interleaving, which is what the trace comparison
sees.

**`tools/diffsh2.py --cpu slave` now walks the whole extract with no fatal
divergence**, where it used to part company for good at reference line 103. The
slave takes the interrupt out of its delay loop, vectors through `0x060001F8`
into the cache-array handler at `0xC0000004`, and comes back, exactly as the
reference does.

Two things had to move for it. **The BIOS comm words are delivered at the
68000's own read** rather than once a frame, because the slave's driver uses
comm 4 for its own traffic and, once the timer runs, writes there 366 times a
frame — the checksum handshake was being trampled before the 68000 ever looked.
The hardware ordering is the same and never collides: the 68000 reaches
`0x8807C2` at reference line 378,847, the slave enters cartridge code at
379,065, and its first PWM interrupt is at 379,271.

And **both SH-2s now stop polling after 1,024 reads instead of 200,000**. Each
one parks on a register only the other CPU can change, and the other CPU cannot
run while it does, so a poll that has come back zero a thousand times will never
come back anything else. The old bound cost two million block entries of nothing
every time either CPU went idle; 200 headless frames went from 0.46s to 0.07s,
with every other number identical.

### The 68000 oracle works past the boot

**52,171 of the reference's 54,081 instructions now agree — 96.5%, with no
fatal divergence anywhere in the extract.** It was 22,037 when this was written
off as "needs interrupt timing modelled", and 32,662 after this session's
earlier fixes. Almost none of the remaining gap was timing.

The frame is run **a scanline at a time** now, with everything timed hanging off
that one clock: the vertical interrupt at line 224, which is where the
reference's own markers put it once the display is up (`Vblank SR=3 @ 224,n`),
and the VDP's VBLANK bit, so a wait loop sees the real thing rather than a
free-running counter that flipped on whichever read happened to land. That was
the change the gap was blamed on, and on its own it moved nothing.

**What actually mattered was the controller.** We modelled no pads at all. The
identification routine at `0x8F45F0` pulses TH, reads the low nibble twice and
maps the pair through a table at `0x8F4620`; an empty port floats high both
times and comes out as index 14, where a pad answers 0xF then 0x3 and comes out
as 12 — which is the `jsr (pc,d0.w)` target `0x8F45E8` the reference takes. We
were taking the empty-port branch and skipping 74 instructions of pad handling,
204 times over the extract. Modelling a pad took agreement to 40,561.

And it is a **six-button** pad. The routine at `0x8F46EE` pulses TH three more
times and branches when the low nibble comes back zero, which is the six-button
signature and which a three-button pad never produces. The reference takes that
branch. Sequencing the pad by TH pulse count took agreement to 52,080.

The last piece was the interrupt vector. The adapter's table names a *stub*
where the cartridge's own header names the stub's target — the cartridge shows
this itself by writing `0x008802A2` into vector 28 at `0x88077E`, and
`0x8802A2` is `jmp 0xffffc036`, exactly what its header holds for that vector.
Vector 30 is the same shape, and the reference executes `0x8802AE` on every one
of its 102 vblanks.

What remains is 244 register differences and 143 trip counts, and the guess
recorded here — that they were the Genesis VDP's vertical-interrupt flag, which
we never set — was wrong on both halves. See the next section but one.

### A gate, and a bound that was costing more than it bought

`make check` runs all of it: both images' byte-exact round-trips, the recompiler
semantics tests, and the four trace diffs. The diffs already exited non-zero on
a fatal divergence; they now also fail when a run **stops short**, which is what
a crash or a truncated trace looks like and which used to read as a clean pass —
the walk simply ended when our stream ran out, recording nothing.

The idle-poll bound came down from 200,000 to 64. Each SH-2 parks on a register
only the other CPU can change, and the other cannot run while it does, so a poll
that has come back zero sixty-four times never comes back anything else. The old
number bought nothing and cost two million block entries every time either CPU
went idle — and the slave's idle handler burns 99 delay steps per poll, so it
was multiplied again in the trace. That also explains the slave's apparent
runaway at `0xC0000132`: 75,923 instructions against the reference's 3 was not a
handler looping, it was the init's idle spin being counted into the interval.

### Three questions, and the two of them that dissolved

The list that stood here was: fix VDP status bit 7 for ~387 of the 554
divergences, then find why our 68000 under-issues command 7, then chase the line
table collapsing after frame ~350. Working down it found that the first was
misattributed, the second was not happening, and the third had already been
fixed. What follows is what is true instead.

**The poll at `0x883254` is not the Genesis VDP.** `tst.b ($a15107)` reads the
*32X* DREQ control register, and bit 7 there is FIFO full, not interrupt
pending. What was actually wrong is that 0xA15106 had no read case at all on the
68000 side, so every read of it came back zero — 100 divergences on its own.

The reference pins two behaviours. 68S, bit 2, reads back for as long as the
transfer is open: the 68000 sets it at `0x88322C`, feeds the FIFO four words at
a time, and reads 0x04 at `0x883250` on every group but the last, where it reads
0. So the word count is taken from the length register on the rising edge of
68S and the bit drops on the final word. FIFO full is never set, here or in the
reference — the SH-2's DMAC is armed before the first word arrives and drains
each one as it comes, and the reference passes `0x883254` 103 times without once
taking the `bmi` back to it.

**The Genesis VDP status register was wrong too, just not there.** The game
reads it three times in the whole extract and we matched none of them:

| | reference | ours, before |
|---|---|---|
| `0x0005B0`, the first read of the run | 0x3288 | 0x0200 |
| `0x0005F2`, waiting out a DMA | 0x0A8A then 0x0A88 | 0x0200 |
| `0x880AAC` | 0x0A88 | 0x0200 |

VBLANK is set in all three, including the first, where the frame loop is on line
0 — because the display is still disabled and a disabled display is in blanking
all frame. The vertical-interrupt flag is set in all three as well, and the
middle one settles how it behaves: it is one read out of 16,555 in the same
loop, all of them with the bit still up, so this VDP does not clear it on a
status read. It clears on the acknowledge cycle, which during the boot never
comes because the 68000 is masked to level 7 throughout — which is also why it
is already set on the very first read, the adapter's boot ROM having run 379,000
instructions of vblanks before the cartridge got control. Musashi's int-ack
callback is on to hook that, and takes over lowering the request.

And bits 10-15, which the VDP does not drive, hold the word the 68000 has
already prefetched. Two distinct values across the three sites and both exact:
0x303C at `0x0005B0`, 0x0800 at the other two.

**554 -> 452 divergences, 52,381 of 54,081 in lock step, and the boot 34 -> 32.**
Every group left is one of three causes and all three are the *synchronous
SH-2* — it runs inside the 68000's register write, so every rendezvous answers
on the first poll:

| rows | where | what |
|---|---|---|
| 144 | `0x883242`, `0x883244` | the command interrupt is acknowledged before the 68000 can see it pending |
| 184 | `0x8802AE`, `0x88314C`, `0x8831B4`, `0x8836E6` | comm 0 is never busy when the vblank handler looks, so we run the palette upload and post a command where the reference skips 22 of 102 |
| 52 | `0x8834C0` | the phase of the vblank wait loop |

That is now the largest single thing left on the 68000 side, and it is a
scheduling question rather than a register one.

**Command 7 is not being under-issued.** The lookup was: find where our asset
loading parts from the reference's. It does not. Both issue command 7 exactly
once over the extract, from the same call site — `0x8ABF28`, the only one of its
eight reached by either — with the same argument, `d0 = 0x51BC`. The premise was
false and there is no 68000-side divergence to find.

The debt underneath it is real, though, and now has a location. Over 1,200
frames the 68000 posts 3 command 7s against 1,173 draws, and the master's sprite
drawer at `0x06001380` and `0x060013C4` reads a 12-byte object header through a
null pointer **10.8 million times**. Whether that is normal for this point in
the game cannot be answered from the logs as extracted: the master's reference
stream is capped at 400,000 instructions and never reaches `0x06001380` at all —
it is still in the boot and the first decompression. The raw logs hold 6.5 GB.

**The line table no longer degenerates.** Not at frame 350, not at 2,000, and
not at the previous commit either — something between the observation and now
fixed it and the note was carried forward stale. "First zero entry" was the
wrong thing to have been watching in any case, since a table can have a hole and
be fine either side. The report counts coverage and distinct offsets now: 128 of
224 lines set, 128 distinct, unchanged from frame 200 to frame 2,000. The 96
lines with no entry are the real gap and they are the same 96 throughout.

**Controller input is in.** The sequencer already knew the whole six-button
protocol; what it lacked was anything to hold. With nothing held it returns
exactly what it returned before, so the trace comparisons are untouched.
`--hold up,start` presses buttons in a headless run, which is the only way the
path can be tested without a person at the keyboard — with `right` held the
game's own identification routine reads 0x77 out of the port at `0x8F4604` where
it reads 0x7F with nothing held.

### The 68000 front end had never been asked what it was missing

`tools/diffsh2.py` ends every run with "every instruction the reference ran is
inside a block we have". Nothing asked that of the 68000. Asked for the first
time — both traces filtered to the addresses discovery has — the answer was 44,
and one of them was the routine that runs every frame.

**Interrupt handlers are installed, not called.** The adapter's vector points at
a stub in the cartridge, the stub is `jmp` through a fixed word of work RAM, and
the engine writes the real address in while it runs:

    883464  lea    %pc@(0x8836d2),%a0
    883468  move.l %a0,($ffc032)          the vertical interrupt's slot

So nothing static reaches `0x8836D2`, and the whole vertical interrupt handler —
which calls the comm-0 poster, the VDP updater and the pad reader, 102 times
over the extract — was outside the front end. The `lea` is PC-relative, so its
target is already a cartridge offset and nothing has to be inferred; seeding
from the pair is exact. Twelve different handlers are written to that one slot,
and the boot fills every trampoline with a `jmp $880B2A` first.

**Two dispatch tables were being given up on**, for reasons that belong to the
walker rather than the tables. `0x8834D6` loads an index that was already scaled
when it was stored, so there is no arithmetic to read the stride off — but an
entry has to fit in its own slot, and slot zero there is a 4-byte `bra.w`, which
settles it without guessing. And a table whose nth case is "do nothing" writes
it as `nop / rts`, which is half of the controller identification table at
`0x8F45D0`, so a slot may hold a return. Between them, the engine's six-case
mode dispatch and the six-button pad path.

**A 68020 field is a reliable sign the bytes are not 68000 code.** Widening
discovery walked into two data regions, and the round-trip caught both. What
they have in common is a brief extension word with the scale field set — which
a 68000 ignores and nothing assembled for one ever emits. It is also the one
thing `m68k-elf-as` refuses outright, so leaving it in fails the round-trip
rather than passing quietly. `looks_like_code` rejects it now.

Tried and rejected: seeding after an unconditional `bra`/`jmp`, the way the SH-2
side does. It adds 73 functions and breaks the whole-cartridge round-trip. On
the 68000 what sits behind a `jmp` is very often the table it dispatches
through — one of the two data regions above is exactly that — and a wrong start
on a variable-length encoding costs what it does not cost on a fixed-width one.

**368 -> 561 functions, 11,655 -> 24,578 instructions, 1.3% -> 2.8% of the ROM**,
both images still byte-exact. The only address either gate trace executes that
the front end does not have is `0x8802AE`, the adapter's own vector stub, which
is not the cartridge's to name.

`make check` now runs both 68000 round-trips — left out for a while, which was
the wrong call the moment the front end started changing — and ends on
`disasm68k.py coverage`, the check that found all of this.

**Then the same question over thirty-three seconds instead of two.** A clean
answer against the gate trace only says discovery has the code *those* runs
reached, and the reference extract is 1.7 seconds of a game. A 2,000-frame run
is 23.5 million instructions and 4,071 distinct addresses against the extract's
846 — and 311 of them were missing. Two more of the front end's own limits:

* **A handler can be two instructions long.** `looks_like_code` wants four
  before it will believe a blind sweep, and the installed-handler sweep was
  inheriting that although its evidence is far better. `0x883710` is `move.b
  #1,($ffffd1)` then `rte`; `0x883BA8`, which runs 1,408 times over those
  frames, is three instructions. Both were turned away by their length.
* **A table's origin can be the instruction dispatching through it.**
  `0x8811D6` is `jmp %pc@(0x8811d6,%d0:w)` — slot zero is the `jmp`, no index
  selects it, and refusing the base threw the whole table away. Three cases,
  one of them entered 12,189 times.

311 -> 245, and what is left is characterised rather than pending:

| where | why |
|---|---|
| `0x8F4992` | `jmp (a3)` — a runtime function pointer, needs dataflow |
| `0x8F4D2A` | a stride-16 table whose slots hold short runs of *code* padded with `nop`, not one transfer — and whose stride comes from `moveq #15 / and / sub`, which rounds the index down rather than scaling it up |
| 28 other runs | cold, executed once or twice each over 2,000 frames |

Recovering the second needs a slot rule weak enough to be worth doubting, which
is a bad trade against a round-trip that is currently exact.

### M6a — the 68000 → C recompiler

The front-end work above was for this. All 561 discovered functions translate,
the result compiles clean for arm64 under `-Wall`, and 38 semantics cases agree
with an independent implementation on both the value and the condition codes.

**The decoder hands out structure now, not only text.** It was built to be
diffed against objdump, so an effective address came out as `%a0@(8,%d1:w)` and
nothing else. `Insn.eas` carries the same parse in machine terms — mode,
register, displacement, index, resolved address, immediate — produced by the one
function that already does the parsing, so the two cannot drift. Half the
two-operand encodings name one side in the opcode word rather than through an
EA, and those are recorded too, in the right position. The text is untouched:
45,496 opcodes still agree with objdump exactly and both cartridges still
reassemble byte for byte.

**Nothing nests, for a different reason than on the SH-2.** There the flat
trampoline was forced by handlers that branch away instead of returning. Here it
is forced by the 68000 keeping its return address in memory: `pea`/`rts`, and a
`jsr` through a table entry, are ordinary things this engine does and none of
them fit a C call. Every transfer returns its destination and `m68k_run` loops.

That needs a call's return address to be a block of its own, which discovery was
not making — the first recompiled test ran exactly one case and stopped, because
the address `rts` handed back had no row in the table. The SH-2 side found the
same thing when it stopped nesting calls. 6,450 -> 7,943 blocks, listing
unchanged.

**Musashi is the oracle, one instruction at a time.** The values a case produces
are easy to write down; V and C are the part a reading of the manual gets wrong.
So `tests/m68k/arith.s` runs twice over identical memory — once interpreted,
once recompiled — and each case records its result *and* the condition codes.
The 38 cover the four add/sub overflow rules, logic leaving X alone, all eight
shifts and rotates including a zero count and one taken modulo 64, `addx`
keeping Z only if it was already set, bit ops modulo 32 in a register and 8 in
memory, `divs` truncation, `movem` both directions with word loads
sign-extending, and an address register taking a full-width add with no flags.

Seven instructions of the cartridge are outside the model and **none is ever
executed**: three `sbcd`, two `movep`, and two words at `0x0000C0` — which is
the adapter's BIOS helper, where the cartridge image holds a vector pointer
rather than the code that runs there, and `src/gen68k.c` assembles the real
thing. Neither the 2,000-frame trace nor the reference reaches any of them.

### It runs the game

`./build/mars --recomp` puts the recompiled code in the 68000's place. At 300
frames it posts 304 commands against the interpreter's 298 and produces the same
picture: 43,314 frame-buffer bytes, 128 of 224 line-table entries, 362 palette
bytes — the same numbers either way. Musashi stays in the build and stays the
default, because it is also what makes the swap possible.

Three things had to be true, and two of them were not.

**Recompiled code and the interpreter live in different address spaces.**
Discovery works in cartridge offsets — the 68000 sees the same bytes through the
direct window and through 0x880000, and an offset is the only stable coordinate
— so the generated code names offsets while a PC handed back by the interpreter
names an address. Below 0x400000 those are the same number, which is exactly why
the boot ran either way and hid it until the engine proper was reached. The
trampoline canonicalises now, and enters the function at the offset rather than
the address it was asked for.

**The first 256 bytes are not the cartridge's.** The game calls the adapter's
BIOS helper at 0x0000C0, and the cartridge image there is a table of pointers,
which is one of the seven things the recompiler will not translate. Recompiled
code returning "no block" for anything below 0x100 sends it to the interpreter,
which reads what `src/gen68k.c` actually assembled there — the same fact the
coverage tool already encodes as its adapter-stub exclusion.

**Some of what the 68000 runs is not in the cartridge at all.** It assembles
routines into work RAM at 0xFF0000 and jumps to them, and no amount of static
discovery finds code that does not exist until run time. So the answer is not a
better front end: a recompiler for a machine like this needs an interpreter
beside it, and the two share an address space and a register file so a handover
costs a register copy. That is what `--recomp` is — recompiled where there is a
block, interpreted where there is not.

### The reference extract goes 14x further

`tools/diffsh2.py --extract-to` names its output, so a longer extract is a
different artefact rather than a newer one and the gate pair stays put. Asked
for 8 million instructions per CPU it exhausts the logs instead: **5,643,099
master instructions and 5,298,292 slave**, against the 400,000 it had.

That reaches everything the debts were waiting on — the frame buffer write at
`0x060021EA` 48,699 times, the sprite drawer at `0x06001380` 238, the command
list interpreter at `0x060009A6` 397 — and settles one of them outright.
**Command 7 is issued once**, at `0x060010D8`, across 5.6 million master
instructions. Our own 300-frame run posts exactly one. The asset table being
barely filled is what this point in the game looks like, not a bug to find.

It also makes (3) below reachable without an emulator: 48,699 frame-buffer
writes logged with the register state that produced them is a reconstruction of
what the real machine had on screen.

### The two CPUs interleave

The SH-2 is preemptible now. The trampoline alone could not do it — a poll loop
inside one function is a `goto` in the generated C and never returns to the
trampoline, which is exactly why this used to need a watchdog that unwound the
whole run. So the suspension point is the basic block, the one boundary every
path crosses and which already carries a hook. `sh2_fuel` is instructions left
in the slice; a block that finds it spent parks the resume address in `c->pc`
and returns `SH2_YIELD`, which is 1 because an SH-2 instruction address is
always even. It is checked before it is spent, so a slice always makes progress.

Both watchdogs are gone rather than retuned — the 20-million-access budget and
the 64-read idle bound. Both existed because a CPU waiting on the other could
only be stopped by abandoning its call; a CPU parked on a poll now spends its
slice polling and yields with its position intact, which is what the hardware
does.

**`rte` was the piece that made it resumable, and the reference settled its
shape.** An SH-2 exception pushes SR and PC and `rte` pops them: the slave's
`r15` goes `0xC0000800` -> `0xC00007F8` as it takes the PWM interrupt with its
mask going 2 -> 6, and the `rte` at `0x06000216` puts both back. `rte` used to
return 0 and end the run, which worked only because no other CPU could be
waiting mid-instruction. `mars_deliver_int` now stacks and redirects and returns
at once; a masked interrupt is declined, which the old model had no way to say.

**A scanline is far too coarse to be the unit, and the boot says why.** The
68000 zeroes comm 0 at `0x880A00`, waits for the master's `0xFFFF`, and posts
its first command at `0x88189C` — sixteen instructions, all inside one
scanline's 487 cycles. The master polls that register every six instructions and
on hardware samples it some fifty times in that window. Run a scanline at a time
and it never sees the window at all: it waits for a zero that has already been
and gone, and the first attempt deadlocked there. Sixteen hand-overs to the line
fixes it.

**The SH-2s are held in BIOS.** Starting them at reset lets the master's
dispatch loop read `M_OK` out of comm 0 and dispatch on it — `r2 = 0x4d5f`,
scaled by 4, straight into nothing. On hardware both are inside the adapter's
boot ROM for the machine's first 379,000 instructions, and that ROM is what
posts `M_OK` in the first place. The hand-over is now where the reference puts
it: the master enters `0x060001A0` 166 log lines after the 68000 reaches the
checksum rendezvous at `0x8807C2`, and *before* the `M_OK` check at `0x8809A6` —
a whole rendezvous earlier than the intuitive place to put it.

**The fuel rate is a measurement, not a guess.** The SH-2s are clocked at
exactly three times the 68000, but fuel is spent in instructions, so the ratio
has to be divided by what an instruction costs. One per cycle is the SH-2's best
case and too generous: the slave's idle loop is pure waiting, so its length is
an observation, and the reference burns 2,826 instructions there where a
one-per-cycle budget ran 5,256. At two cycles an instruction it runs 2,616.

*Gate met:* `make check` passes — both cartridges still reassemble byte for
byte on both front ends, all 8 SH-2 and 38 68000 semantics cases still agree,
and all four trace diffs walk their whole extract with no fatal divergence. The
picture is unchanged: 43,314 frame-buffer bytes, 128 of 224 line-table entries,
the same 32X image as before.

What this bought, and what it cost — the second column is the honest half:

| | before | after |
|---|---|---|
| 68000 boot | 20,062 agreed, 32 div | 19,672 agreed, **24** div |
| 68000 whole extract | 52,381 agreed, 452 div | **35,412** agreed, **525** div |
| master, boot | 158 of 177 blocks, 19 div | 134, 41 div |
| slave | 17 blocks, 6,722 div | 17 blocks, 11,304 div |
| distinct 68000 PCs we execute | 841 | **887** |

The boot improved and the game runs more code than it did. Everything else got
worse, and pretending otherwise would waste the next session:

* **The 452 divergences the previous section attributed to the synchronous
  SH-2 did not collapse.** They moved — `0x883244` halved, `0x88314C` went, a
  new group of 100 appeared at `0x8836F6` — and the total rose. That
  attribution was wrong. What dominates now is the **vblank phase**: the
  reference takes its vertical interrupt at a point in the engine's wait loop
  at `0x8834C0` that we reach with a different mask and a different `d1`.
* The slave's count rose because far more of it is now *being compared*: it
  used to be unwound after 64 idle reads, so most of its blocks never
  happened. 17 blocks agreeing out of 75,342 is the real number and always was.
  Its interrupt entries at `0x060001F8` differ in register state, which is the
  PWM phase — we deliver on line boundaries where the reference delivers on
  cycles.

So this is a change of kind, not yet a change of quality: the CPUs interleave
the way the hardware does, and what is left is *when* each interrupt lands
rather than whether a rendezvous is answered at all.

Two measurement facts fell out, and both are now in `make check`. The alignment
window has to exceed the longest wait, because our 68000 spins 65,270 times at
`0x881A06` where it used to spin none — at the old 40,000 a genuine rejoin read
as a fatal divergence. And the trace has to outlast the idling: at 400,000 lines
per CPU, 393,634 of the master's were the single poll block at `0x060008F8`, so
the run read as having stopped short. Neither was a divergence.

**And the recompiled 68000 needed the same yield, for the same reason.**
`--recomp` hung outright, and a sampled stack named the spot: inside
`m_001868`, which is `0x881868` — the engine waiting for the master to post
`0xFFFF`. Its budget was a count of control *transfers*, which cannot bound a
poll loop at all, because an intra-function loop is a `goto` in the generated C
and never returns to the trampoline. That was invisible for exactly as long as
the SH-2 answered inside the 68000's own register write. It is fuel now, spent
in the block prologue like the SH-2's, and the slice converts cycles to
instructions at 8 cycles apiece rather than being untethered from the clock.

The two backends agree more closely than they did: 249 commands against
Musashi's 247, where it was 304 against 298, and the same frame buffer to the
byte. Handovers went the other way — 78,898 to **1,147,341**, about 91% of all
sub-slices — because a hand-over returns wherever Musashi stopped, which is
usually mid-block where no row exists, so the next slice hands over again. That
is (4) below, now with a number worth chasing.

### The 68000 keeps time

The vertical interrupt was raised at line 224, given a fixed 2,000 cycles, and
then lowered — so a 68000 masked across that window lost the frame's vblank
outright, and the 2,000 cycles were 1.5% of a frame the machine never had. It is
*held* now, offered at every hand-over until taken, and dropped where the
hardware drops it: the acknowledge cycle, which `gen68k_int_ack` already hooked.

That left the real question, which the diff had been stating plainly all along
and which is a pacing question rather than an interrupt one: **between two
vertical interrupts the reference runs 11,232 instructions and we ran 12,230**,
every frame, stable to the instruction.

*Tried and rejected: VDP bus contention.* The theory was that the VDP stealing
cycles from the 68000 during active display was the missing 8%, which is real
hardware behaviour Musashi knows nothing about. Modelling it changed the count
by **nothing at all** — and that is what identified the actual cause, because a
budget that can be cut by a tenth with no effect was not what governed.

**`m68k_execute` runs whole instructions.** It overshoots whatever it was asked
for and returns what it actually spent, and that difference was being thrown
away. Discarding it once a scanline is harmless; discarding it sixteen times a
line makes the overshoot, not the request, set the clock. `cpu_credit` carries
the debt, so a slice's request is a rate again — overspend now and the next
slices are shorter until it is paid back. 12,230 -> 11,032.

The last 1.8% was arithmetic. `CYCLES_PER_LINE` is 487 where the figure is
487.9, and dividing that by sixteen truncates again to 30 from 30.4 — 2,080
cycles a frame between them. Handing `CYCLES_PER_FRAME` out across exactly the
frame's hand-overs loses nothing: **11,224 to 11,249 against the reference's
11,232 to 11,240**, which is 0.1%.

What that does to the diff is not a smaller number of divergences — it is 532
where it was 525, because a trip count off by one is a row exactly like one off
by 998. It is that the rows changed size. Trip differences are now ±1 to ±18
almost everywhere, where they were hundreds to thousands, and the large ones
left are the four BIOS stand-ins that answer instantly by construction: the
checksum (672,348), the VDP DMA busy bit (16,554), and the two comm rendezvous.

Two smaller things came out of it. `gen.vint_pending` was doing two jobs —
VDP status bit 7, which the adapter's boot ROM leaves set before the cartridge
runs an instruction, and the request on the 68000's interrupt lines, which only
a real vblank raises. They are separate fields now, because holding the request
meant the boot's stale status bit would have fired an interrupt the moment the
mask dropped.

And **the front end was missing a one-instruction handler.** With the vblank
held, our 68000 takes one in the window between the boot filling its trampolines
and the engine installing its own handler, and runs the `rte` at `0x880B2A` —
which the reference never reaches, because its boot is a million instructions
slower where our stand-ins answer at once. `looks_like_code` wants four
instructions before it will believe a blind sweep, and this is one. The evidence
is better than length, though: `rte` cannot appear anywhere but as the last
instruction of an exception handler, so an `rte` sitting where control cannot
fall through is a handler by construction. 24,578 -> 24,579 instructions, both
images still byte-exact, and `coverage` clean again.

### The slave gets its clock properly

The other half, and the one that moved the most. The PWM timer is the slave's
only clock, and it was being scheduled as *a count per frame spread over the
scanlines* — which is wrong twice. A count per frame has to be an integer and
this one is 365.96, so it was 365, losing an interrupt every four frames. And
spreading a frame's worth over 262 lines quantised each one to a line boundary,
up to seven hundred SH-2 instructions from where the timer would have fired it.
Taking one also handed the slave an extra slice on top of its clock share,
about 9% of free fuel, which is not what an interrupt does — it redirects the
CPU, and the ordinary slice runs the handler.

`mars_pwm_period()` returns the period itself, `(cycle + 1) * TM` in SH-2
cycles, and the frame loop counts SH-2 cycles against it. Both SH-2 cycles and
68000 cycles are now handed out across the frame's hand-overs with the
remainder distributed rather than truncated, so all three CPUs run off one
clock and nothing is lost to integer division.

**The slave goes from 17 blocks in lock step to 269**, the first movement on it
since there was anything to measure, and it takes the 68000 with it: 532
divergences to 500, because the two share a machine. Its idle loop lands at
3,045 instructions against the reference's 2,826 — 7.7% over, where the errors
before this were the loop being abandoned after 3.

Still open: 162 register differences and 83 places control flow parts on the
68000, led by `0x8836F6` and `0x883244`; and on the slave the interrupt entries
at `0x060001F8` that still disagree in register state.

### A trace that survives idling

Both gates were buried in their own idling: 3.3 GB of trace across the two
files, of which the slave's 2.67 million lines on one `dt`/`bf` delay loop and
the master's 393,634 on one poll block were the bulk. Both tracers now collapse
a run of the same instruction — the same block, for the SH-2 — in the same
unchanged state, into the reference tracer's own `[Omitted: N]`, which
`tracediff` already reads. `diff68k` and `diffsh2` fold the count back into the
preceding record's instruction total, so a collapsed spin still reports its trip
count rather than reading as having run once.

Nothing is lost by construction: every line left out would have been a copy of
the one written, and the 68000's two diffs come back **bit for bit identical**
across the change, which is what says so.

**Collapsing on the address alone was tried and is worse.** It folds runs the
reference prints in full — the slave's delay loop counts r0 down, so its entries
differ and the reference logs each one — and with fewer records than the
reference has, the aligner cannot find the four consecutive matches it wants.
The slave fell from 269 blocks in lock step to 196 on it. Requiring the state to
be unchanged as well keeps the master's win and costs the slave nothing.

3.3 GB -> **1.35 GB**, and the master's agreement went *up*, 129 blocks to
**193**, because a budget now buys game time instead of copies of an idle poll.
The 68000's budget is deliberately past what the run produces: the collapse is
what does the shrinking, and capping on top of it only costs `coverage` the
breadth it exists to measure — cutting it in half saved 91 MB and 14 distinct
addresses, which is the wrong way round.

### Next

**1. What is left of the phase**, now that the clocks are right: 162 register
differences and 83 places control flow parts on the 68000, led by `0x8836F6`
and `0x883244`, and the slave's interrupt entries at `0x060001F8`. These are no
longer trip counts, so they want reading one at a time rather than a mechanism.

**3. The screenshot check.** The one real verification gap in the rendering
path — nothing has confirmed the packed-pixel decode, the palette conversion or
the frame-select polarity against ground truth. This no longer needs an
emulator: the extended extract logs 48,699 executions of the frame buffer write
at `0x060021EA`, each with the register state that produced it, so replaying
them reconstructs what the real machine had on screen.

**4. Cut the handovers down.** `--recomp` hands to the interpreter 1,147,341
times in 300 frames, about 91% of all sub-slices, where only about one per frame
should be structural — the genuine case is the handful of routines the engine
assembles into work RAM at `0xFF0000`, which no static front end can find. The
suspicion is a feedback loop rather than that many real gaps: a hand-over
returns wherever Musashi stopped, and an arbitrary PC mid-block has no row in
the table, so the next slice hands over again whether or not the address was
ever unreachable. Only the first is recorded; a histogram is the first thing to
build, and re-entering at the *containing* block rather than requiring a leader
is the likely fix.

The recompiled 68000 also has no trace gate at all — `src/trace68k.c` hooks
Musashi and reads Musashi's registers, so under `--recomp` it logs only the
interpreted fragments, and the mode is held to nothing but "same picture, same
command count". The per-block hook the SH-2 side uses is the same two lines of
codegen, and the blocks now carry a prologue to put it in.

### Carried debts

* ~~**The asset table is barely filled.**~~ *Settled.* The extended extract
  shows the reference issuing command 7 exactly once across 5,643,099 master
  instructions, and our 300-frame run posts exactly one. A mostly-empty asset
  table is what this point in the game looks like. The 10.8 million reads of
  address 0 are the sprite drawer walking slots that are legitimately unfilled.
* **96 of the 224 lines have no line-table entry**, so the 32X draws the top 128
  and the Mega Drive shows through below. Stable, not a collapse.
* **Interrupt phase.** The CPUs interleave now, but the vertical interrupt is
  raised at a fixed point in the line and the PWM interrupts on line boundaries,
  where the reference has both on their own clocks. It is what is left of the
  68000 diff and most of the slave's.
* **No audio.** The sample FIFOs accept writes and report space so the driver
  never stalls, and the samples go nowhere.
* **Genesis DMA fill and copy are skipped.** The reference issues 65,535 fills
  during boot; `vdp_dma` returns early on both.
* **The 32X frame-select polarity is a guess.** Displayed is taken to be
  `fb[FS]` and the CPUs get the other one; nothing has confirmed which way round
  it is.
* **Nothing has been checked against a real screenshot.** Both pictures are
  plausible — a Chaotix level from the Mega Drive half, decompressed art from
  the 32X half — which is not the same as a verified match. The packed-pixel
  decode, the palette conversion and the frame-select polarity all rest on it.
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
