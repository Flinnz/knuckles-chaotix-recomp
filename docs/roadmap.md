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

*Later: 4,491 functions and 7,944 blocks, once orphaned blocks were adopted —
see "Half the front end was not reachable" below.*

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

### The picture gets an oracle, and it was wrong

An instruction trace looks like the wrong tool for checking pixels, being a log
of CPU state rather than of memory. It is not: every store is in there with the
register state that produced it, so the writes replay. `tools/refframe.py` runs
the master's 5.4 million instructions back through a model of the 32X address
space and reconstructs the frame buffer, with no emulator involved, and
`build/mars --dump-32x` writes ours in the same layout to compare.

**The line table was being destroyed by our own autofill.** The reference
settles what the table should be without any comparison at all, because the
code that writes it is right there — `0x060031B0` loads `0x24000200` into r8,
`0x100` into the counter, and `0x00010000` into r0, then walks *backwards*
storing r0's low word and subtracting `0x100`. So entry *k* is `(k+1) * 0x100`,
all 256 of them, and every one of the 224 on screen is non-zero. Ours had 128.

The half that was missing is byte offsets `0x100`-`0x1FE`, which the loop writes
*first* — so they were not missing, they were overwritten afterwards. The fill
start register holds a **word** address, not a byte one: sixteen bits reach all
65,536 words of the 128 KB buffer exactly, where as a byte address they would
reach half of it. Taking it for a byte offset put every fill at half the address
it belonged at, and what that landed on was the line table. The address also
increments in its low 8 bits only, so a fill wraps inside a 256-*word* block,
which is what makes it useful for clearing one scanline of a 512-byte-stride
buffer; stepping a byte address by two did not do that either.

*Gate met, in the only way this one can be:* **the 32X picture at 300 frames is
the SEGA logo**, drawn correctly, where before the fix it was cloud panels over
a half-clobbered table. That is the first time anything in the rendering path
has been checked against something other than "it is not empty". The line table
is 224 of 224 entries and matches the reference's own arithmetic exactly.

This also closes a carried debt by contradicting it. "96 of the 224 lines have
no line-table entry, so the 32X draws the top 128 and the Mega Drive shows
through below. Stable, not a collapse" — stable it was, and a bug all along.

**What the replay cannot do**, and it was not obvious: the reference tracer
collapses tight loops, printing the first iteration and eliding the rest, so
stores inside them were never logged. The line-table writer is the clearest
case — 256 iterations an invocation, six lines in the whole extract, every one
of them the first iteration with r8 still `0x24000200`. A reconstruction is
therefore a lower bound, and where a loop is elided the arithmetic has to be
read out of the code instead, which is what happened here. Its frame buffer
agrees with ours on 79% of bytes and holds neither a line table nor a palette,
both of which are written in loops it elided; that number is a floor, not a
score.

### Half the front end was not reachable, and only `--recomp` could tell

1,136,315 hand-overs to the interpreter in 300 frames, about 91% of all
sub-slices. A histogram of where they go — the first thing the plan asked for,
because only a count per address tells a million real gaps from a handful of
addresses hit over and over — settles it in one run: **two addresses are 89% of
them**, `0x8834C4` and `0x8834C0`, which is the engine's `tst.b ($ffffd1)` /
`beq` wait for the vertical interrupt.

Neither has a recompiled block. Nor does anything else in that region: the
dispatch table held **4,085 blocks where the front end had traced 7,943**.

A function is built by walking `succs` from a registered entry, so a block that
discovery traced but that no entry reaches falls out of `funcs` entirely. It is
still in `code`, so the listing has it and the round-trip covers it and
`coverage` was satisfied — and the recompiler, which emits one C function per
*function*, never saw it. Function `0x3334` ends at its `rts` and everything
from there to `0x36D0` was orphaned, the vblank wait among it.

Adopting an orphan as its own entry cannot affect the round-trip, which is
emitted from `code` rather than from functions, and cannot mis-attribute
anything, because a block's exits are explicit and any owner will do. 562
functions become 4,491, and 4,085 dispatchable blocks become 7,944.

**1,136,315 hand-overs -> 293**, and the run is 2.4x faster. What is left is
exactly the structural remainder the plan predicted: 279 at `0x8802AE`, the
adapter's own vector stub, which is not the cartridge's to name and which the
runtime assembles — one per vblank — and 9 in work RAM at `0xFF0000` and
neighbours, the routines the engine builds at run time and that no static front
end can ever find. Same 248 commands, same frame buffer, same line table.

**`coverage` now asks the question that would have caught it.** It checked that
the disassembler knew the bytes; it checks that a block is owned as well, which
is what the translated build actually needs. It also reports the two counts
side by side, so the gap is a number rather than a silence.

*Also fixed, and worth keeping even though it moved nothing here:* a hand-over
used to give the interpreter a whole slice and take back wherever it stopped,
and an arbitrary stopping PC is almost never the start of a block — so the next
slice handed over again whether or not anything was really missing. It now runs
to the end of the *gap*, stopping as soon as the PC is dispatchable. That makes
a gap cost one hand-over however long it is; it did not help the vblank wait
only because that gap never ended.

### The recompiled 68000 gets a gate, and it was running 42% fast

`--recomp` was held to "same picture, same command count" and nothing else.
That was tolerable while the interpreter did nine tenths of the work; with 291
hand-overs in 300 frames, essentially the whole run was untested code.

`src/trace68k.c` hooks Musashi and reads Musashi's registers, so under
`--recomp` it saw almost nothing. The SH-2 side answered this years of commits
ago and the answer carries over: hook the **block**, which is the only place
translated code can be hooked, and filter the reference to those addresses —
the reference logs every instruction, so filtered to the block starts its
stream *is* ours. `M68K_BLOCK` already existed for the fuel, so the hook is one
line inside it; `diff68k.py --blocks` does the filtering, folding the
reference's addresses to cartridge offsets the way `src/m68000.c` does, because
recompiled code names offsets and the same bytes are reachable through more
than one window.

*Gate met:* 4,800 of the reference's 9,848 block entries agree, **no fatal
divergence**, and it walks the whole extract. The only addresses the reference
executes that we have no block for are the nine structural ones — `0x8802AE`
and the `0xFFC030` trampoline it jumps through, and the routines the engine
assembles in work RAM at `0xFF0000` — 216 instructions in all, none of them the
cartridge's to name.

**And it immediately found something.** `recomp_cpi`, which converts a slice of
cycles into a count of instructions for the translated code, was 8 — guessed
from the manual, the cheapest register-to-register forms being 4 cycles and a
memory operand 8 to 12. The reference measures it instead: 11,236 instructions
between two vertical interrupts, 127,840 cycles to a frame, **11.4 cycles an
instruction** for this engine's actual mix. At 8 the recompiled 68000 was
running 42% more instructions a frame than the interpreted one, which is exactly
the class of thing "same picture, same command count" cannot see.

One caveat on the numbers it reports: in block mode the trip-count column runs
about one block's length high, because the interval it sums includes the
previously-matched record. It is consistent, it does not affect the agreement
count or the divergence classification, and it is worth fixing in
`tools/tracediff.py` — where `diffsh2` shares it and has been quietly diluting
it in long idle loops.

### The trip counts were double, and it was not the interval

The caveat recorded above — that `tracediff` sums an interval one record too
wide — was wrong, and reading the numbers rather than the code is what said so.
The rows it produced were not one block high, they were **exactly twice**:
4 against 2, 6 against 3, 152 against 76, over and over. An off-by-one interval
cannot do that. Something was counting every block twice.

`M68K_BLOCK` traced *before* it checked the fuel. A block that finds the slice
spent runs none of itself — it parks its own address and returns — and is
entered again from the top next slice, so it was logged once for the entry that
ran nothing and once for the entry that ran. The two records are identical by
construction, which is exactly why the alignment never noticed: it matched
either one and walked on. Only the instruction counts, which are what a trip
count is made of, came out doubled.

It matters this much here because a sub-slice is **about 2.7 instructions** of
this engine's mix — 127,840 cycles over 4,192 hand-overs at 11.4 cycles an
instruction — so nearly every block was ending one slice and starting the next.
`SH2_BLOCK` had the same order and the same bug, diluted rather than doubled:
an SH-2 slice is some ten blocks long, so roughly a tenth of its entries were
duplicates.

Checking the yield before the trace is the whole fix, and the interval in
`tracediff.py` is left alone because it was right: both sides measure from the
previously matched record up to this one, the earlier record's steps included
and this one's excluded, which is what `gap + 1` counts on the reference's side.

| | before | after |
|---|---|---|
| recompiled 68000, `--blocks` | 4,800 agreed, **8,408** div | 4,800 agreed, **475** div |
| slave | 269 agreed, **11,969** div | 269 agreed, **6,853** div |
| master, boot | 193 agreed, 20 div | unchanged |
| 68000 interpreted | 35,412 agreed, 500 div | unchanged |

Nothing moved on the two interpreted diffs, which is the control: Musashi's hook
is per instruction and never had the bug. What is left on the recompiled 68000
is 475 rows that mean something — the four BIOS stand-ins that answer instantly
(the checksum's 672,348, the VDP DMA busy bit's 16,554, the two comm
rendezvous), and then real ones.

The other half of the fix is free and worth noting: an `[Omitted: N]` line
counts against the trace's line budget, so deleting the spurious ones buys game
time. The slave's trace covers more of the run in the same 1,000,000 lines.

### The SH-2s were running at half speed, and the logs said so in one command

Reading the residual divergences turned into measuring a clock, because the
first one read was a clock. The 68000 enters its vertical interrupt 124
instructions further into the engine's work than the reference does, every
frame — and asking why led to the question nobody had asked: how many
instructions does each CPU actually execute in a frame?

The logs answer it directly. They interleave every CPU and mark the 68000's
vertical interrupts, which is a frame boundary all three share, so counting the
lines between two marks — the collapsed runs included, or an idle loop vanishes
— *is* the measurement. `tools/refrate.py` does that in four seconds. Over
eighteen steady frames:

| | instructions a frame | cycles an instruction |
|---|---|---|
| 68000 | 11,598 | 11.02 |
| master SH-2 | 235,076 | 1.631 |
| slave SH-2 | 380,695 | 1.007 |

The 68000 confirms `recomp_cpi = 11`. The SH-2s were both on a flat 2, so the
master was running at 80% of its true rate and **the slave at half**. The slave
is the stark one: its whole clock is the PWM interrupt every 1,048 SH-2 cycles,
and the reference runs 1,040 instructions between two of them where we ran 524.

The old figure was not a guess either — it was measured, on the wrong thing.
"The slave's idle loop burns 2,826 instructions where a one-per-cycle budget ran
5,256" is a *boot* rendezvous, and how long the slave spins there depends
entirely on when our 68000 answers, which during the boot is when a BIOS
stand-in answers rather than when the real BIOS would have. Two errors
cancelled. A whole frame, counted eighteen times, has nothing to cancel against.

The two SH-2s differ because their work does — the slave sits in a delay loop in
the cache array with nothing to stall on, the master decompresses cartridge art
— so the rate is per CPU, carried per thousand cycles with its remainder so the
division does not truncate 4,192 times a frame.

| | before | after |
|---|---|---|
| 68000 whole extract | 35,412 agreed, 500 div | **42,256** agreed, **434** div |
| recompiled 68000, `--blocks` | 4,800 agreed, 475 div | **5,885** agreed, **382** div |
| slave | 269 agreed, 6,853 div | 173 agreed, 6,766 div |
| 68000 boot, master boot | unchanged | unchanged |

Both 68000 diffs gained about a fifth more agreement, because a rendezvous is
only answered at the right moment if the CPU on the other side is running at the
right speed.

**And the slave's remaining divergences are now one thing, twice.** 3,359 of them
are register state at the interrupt entry `0x060001F8` and 3,336 are the flow
rows that follow from it; there is exactly **one trip-count row in the whole
extract**, where before the counts were wrong everywhere. What they say is that
the reference takes the PWM interrupt at the *same point in the idle loop every
single time* — all 3,360 entries have `r0 = 0` and `pr = 0x060002E2`, which is
the top of the dispatch loop, and the loop is 206 instructions long and runs
five times between interrupts. `r0` is zero for about five of those 206
instructions, so landing there 3,360 times out of 3,360 is not chance: the
reference *defers* the interrupt to that boundary. Idle-loop handling in the
emulator is the likeliest reason, and either way it is not something a faithful
model reproduces without knowing the rule — so this group is probably the
oracle's shape rather than ours to fix.

### The picture was reading its palette out of 68000 code

The next divergence down the list settled the whole of (3) — both halves of it —
and it took one register that two CPUs disagree about.

`0x8836F6` differs on all 102 vblanks, and the delta is **one register, the same
two values every time**: `d1 = 0x1C00` in the reference against `0x1D7C` in
ours. It is the value the handler restores from the stack, so it is whatever the
interrupted code had, and the code that sets it is a loop at `0x88349E` reading
words out of a table at `0x9299D6` and queueing each one through `0x88313E` —
which appends to the list at `0xFFD860` that the vblank handler uploads into the
32X palette at `0xA15200`. So d1 is a *colour*, and ours was the wrong one.

`0x900000` is the banked cartridge window. Read at bank 0, the 38 words there
are `B06E 002E 6618 116E 0031 0031 306E 002C …` — which is not a palette, it is
68000 code: `4E75` is `rts`, `306E 002C` is `movea.w (44,a6),a0`. Read at bank 2
they are `0000 7E00 7E80 7F00 7F80 7FE0 7FFF …`, an unmistakable ramp of 15-bit
colours. The reference's own reads settle which: taking every `movew %a1@+,%d1`
in that loop out of the trace and looking up what it loaded, **38 of 38 match
the cartridge at 0x200000**, none at 0x000000.

The bank is set by `move.b #$02,($a15105)` at `0x8809EE` — and we were doing
that correctly. What undid it is that **`mars.bank` was one field serving two
registers**. The 68000's `0xA15104` is the ROM bank; the SH-2's `0x20004004`, at
the same offset in its own register block, is the **H Count**. They are
different registers on the machine and were the same `uint16_t` here, so every
time the SH-2 set its H count the 68000's cartridge window snapped back to bank
0 — and the engine went on uploading compiled code as colours.

*Gate met, and it is a picture rather than a number:* at 300 frames the 32X half
draws **the SEGA logo in blue with a white outline, a "TM", and the magenta
nebula panels either side of it** — the Knuckles' Chaotix intro screen, correct.

It moves the traces as much as the picture. Reading the right bytes changes what
the engine computes, and **53,451 of the reference's 54,081 instructions now
agree exactly — 98.8%**, where it was 42,256 before this and 35,412 at the start
of the day. The boot goes 19,672 to 20,078 of 20,178.

| | start of day | after the rate fix | after the bank fix |
|---|---|---|---|
| 68000 boot | 19,672 of 20,178 | 19,672 | **20,078** |
| 68000 whole extract | 35,412 agreed, 500 div | 42,256, 434 | **53,451**, 388 |
| recompiled 68000, `--blocks` | 4,800 agreed, 8,408 div | 5,885, 475 | **9,025** of 9,848, 359 |

**The slave's gate is bounded now, and the reason is worth writing down.** With
the palette right the slave tracks the reference far more closely — 911 blocks
in lock step through the first quarter-frame, where the whole extract used to
manage 173 — and that is exactly what defeats the long walk. Our stream is 93%
delay-loop entries, one per iteration, where the reference collapses each run
into a line and a count; two streams of such different densities cost the
aligner its whole budget searching, and six million lines of ours reached 0.6 of
a reference frame. `--ref-blocks 2000` covers the boot, the first PWM interrupts
and the sound driver's first work, and walks it cleanly.

Collapsing our own runs on the address, which is the reference's own policy, was
measured again here rather than assumed: it does buy the slave more extract per
line, and it costs the master a quarter of its agreement — 193 blocks to 143 —
because the master's poll and its work share addresses in different states.
Lifting the bound properly wants the *phase* fixed, not the tracer.

Two carried debts close with it, neither needing the screenshot they were
waiting on:

* **The palette conversion is right.** `col & 0x1F` is red and `(col >> 10) &
  0x1F` is blue, and the proof is that the SEGA logo comes out blue and the
  nebula magenta rather than the other way round. Read the other way this same
  frame buffer is a red logo, which is what it was.
* **The frame-select polarity is right.** With a correct picture to look at, the
  two buffers can be told apart: the one we display holds the whole logo and the
  one the CPUs draw into holds a *partial* redraw of its bottom edge. Reversed,
  the display would be the torn half. That is double buffering working, and it
  is evidence rather than assumption.

The H count now has its own field and does nothing else, which is honest: we
deliver no horizontal interrupt to either SH-2, and the reference takes none in
the extract — every one of its 3,360 slave interrupts is the PWM at level 6.

### The recompiler is the default

`--recomp` was a flag on a build that Musashi drove. It is the other way round
now: translated code runs the 68000 unless `--interp` asks for the interpreter,
and both produce the same picture to the byte — 15,116 frame-buffer bytes, the
same 224-entry line table, the same palette — from 281 commands against 280.

What made the flip defensible is not that the hand-overs got rare, it is that
the translated build has a *gate of its own*: `diff68k.py --blocks` holds it to
the reference at block granularity, it walks the whole extract, and 9,025 of
9,848 block entries agree. "Same picture, same command count" could never have
said that, and it was the only thing saying anything for a long time.

Musashi does not go away, and the honest description of what is left for it is
not "a fallback" but **the code that did not exist at build time**: the
adapter's stubs below `0x100`, which `src/gen68k.c` assembles, and the routines
the engine builds in work RAM at `0xFF0000` and jumps to. 295 hand-overs in 300
frames, all of them those. No static front end can find either, so this is the
end state rather than a step toward one.

### The 32X VDP still had a free-running counter where the beam should be

Chasing the vblank phase went through three things that were not it before
finding the one that was.

*The frame length is right.* Between two vertical interrupts we run 11,611
instructions and the reference runs 11,609 to 11,615 — and a whole frame's
histogram, ours against theirs, holds the same 202 addresses with a total
difference of **+7 instructions**. Whatever is left is not the clock.

*Finer hand-overs are worse, measured twice.* 32 sub-slices to the line gives
53,447 agreed and 64 gives 53,444, against 16's 53,451. And *reordering* the
hand-over so the master runs before the 68000 rather than after flips the
command-interrupt group from "we answer one poll early" to "one poll late" —
1 against 3 becomes 3 against 1 — which brackets the truth from both sides and
lands on neither. A cooperative scheduler has a granularity floor and this is
it.

**What was actually wrong is that the 32X VDP's status register was still a
counter.** `vdp_status()` set VBLK for eight reads in every sixty-four and HBLK
for two in every eight — the stand-in the Genesis side had before it got a
scanline clock, which nobody came back for. It is not cosmetic, because the
master synchronises its whole video init to that bit: at `0x06003116` it waits
for VBLK to clear and then to set, which is "wait for the next vertical blank",
and the reference spends **263,792 instructions** there where we spent 220.

FEN was the same kind of thing and the reference measures it exactly. The
clear loop at `0x06003188` starts an autofill and waits for FEN to drop, 256
times an invocation, and the trace shows 304 instructions per fill — about 99
turns of a three-instruction poll, or **two SH-2 cycles a word**. Ours was
`mars.fbctl & 2`, the bit the CPU had written, which is never set. *Tried and
rejected:* FEN as the display period itself, which is what a first reading of
the 79,576-instructions-per-fill figure suggests — that figure divided by six
*logged* fills where there were 256 real ones. It stalls 197,660 instructions at
`0x06003106`, where the reference reads the same register once and walks on.

| the master's own waits | before | after | reference |
|---|---|---|---|
| `0x06003126`, wait for vblank | 220 | **185,384** | 263,792 |
| `0x0600319C`, wait for the fill | 2,550 | **87,345** | 77,658 |

And the numbers this is held to, which are not all in the same direction:

| | before | after |
|---|---|---|
| 68000 boot | 20,078 agreed, 26 div | 20,077, **20** |
| master | 193 blocks, 20 div | **194**, **19** |
| slave | 912 blocks, 473 div | **1,037**, 546 |
| 68000 whole extract | 53,451 agreed, 388 div | 52,386, 449 |
| recompiled 68000 | 9,025 blocks, 359 div | 8,736, 398 |

The two whole-extract numbers got worse and the cause is identified rather than
guessed at: **our master reaches the same place sooner.** From reference line
40,553 onward the engine finds comm 0 busy at *every* vblank and skips the
palette upload; ours never does, and the queue at `0xFFD860` that the upload
drains holds 32 entries in the reference against one in ours.

*The two figures first written here — "83% of ours are the idle poll against 49%
of theirs", and a video init costing 4.3 frames against 1.5 — were wrong, and
the next section is what replaced them.* They came of comparing our block-entry
counts against the reference's instruction counts, which is not a comparison at
all.

### The master has no missing work; the 68000 crosses the adapter too cheaply

"The master idles where the reference works" was the wrong question, and asking
it properly took one honest measurement: weight our block entries by the block's
length before comparing them against the reference's instruction counts. Over
the same **3,573,959 master instructions** the idle poll is 51.1% of the
reference's and 39.1% of ours — not 49 against 83 — and the master's init is
**374,279 instructions against 439,956**, both doing exactly three vblank syncs
and six autofill loops.

Then the work itself, item by item. Both masters decompress the same twelve
assets, in the same order, from the same cartridge addresses to the same SDRAM
destinations, called from the same site — and **the eleven gaps between them
agree to within one instruction**: 87,912 against 87,911, 36,337 against 36,336,
103,440 against 103,439, and so on down. There is no missing work and no extra
work. The master's behaviour is the same behaviour.

What differs is *when it is asked*. Timed against the master's own clock, from
the moment the BIOS hands it the cartridge:

| | reference | ours |
|---|---|---|
| the 68000 zeroes comm 0 | +3,023 instructions | +123 |
| the 68000 posts its first command | +15,482 | +13,158 |
| the 68000 raises the command interrupt | +95,415 | +93,747 |
| …which in *time* is | **12.22 frames** | **9.76 frames** |

The 68000 runs the same instructions — 93,747 against 95,415, 1.7% apart — and
arrives **2.5 frames early**. The instruction counts are right and the clock is
wrong, which is the opposite of the usual failure.

**The missing time is the adapter.** In the steady state our frame is 11,611
instructions against the reference's 11,609-11,615, exact; through this phase
ours is 9,605 a frame against 7,808, which is 13.3 cycles an instruction against
16.4. What separates the two phases is what the 68000 polls — the engine's own
flag in work RAM afterwards, and the **32X comm register** here. Timing one of
those polls against the master's clock settles it: the `tst.w ($a15120)` / `bne`
pair at `0x8818A4` runs 9,768 instructions in 292,723 master instructions, which
is 159,190 68000 cycles, or **16.3 cycles an instruction**. Musashi charges 26
cycles for that pair — 13 apiece. A 68000 read of a 32X register costs about
**six or seven cycles more than a read of RAM**, and we charge nothing for it.

*One number, measured — and measured against the wrong clock. The next section
is what replaced it, and the residue turned out to be ours rather than the
adapter's.*

### There is no adapter penalty; the clock that found one was the wrong clock

`tools/refpoll.py` is the scanner the plan above asked for, and the first thing
it did was retire the finding that motivated it.

The 16.3 came of timing a boot window by the master's instruction count at 1.631
cycles an instruction — the rate `tools/refrate.py` measures over *steady*
frames. The master does not run at that rate here. Through that poll it is
**1.30**, because it is spinning in a loop of its own rather than decompressing
cartridge art. Scale 12.97 by 1.631/1.30 and 16.29 falls out, which is the
number that was written down.

So the question is what to time the boot *with*, and the phase where the answer
was wrong is the phase with the fewest clocks in it. Three candidates, two of
which do not survive contact:

* The 68000's **taken vertical interrupt** is exactly 127,840 cycles from the
  next — and the engine boots with interrupts masked. 937,053 instructions go by
  without a single marker.
* The **VDP's own vblank line** is logged whether or not the CPU takes it, which
  sounds like exactly the missing clock and is not: 72 of them arrive in bursts
  with no instructions in between.
* The slave's **PWM interrupt** is a hardware timer, it does not care what any
  CPU is doing, and it ticks 367.2 times a frame — 348.1 68000 cycles apiece,
  fine enough to time a run of a few thousand instructions. It stops only when
  the slave does, so every figure derived from it is reported with its tick
  count next to it: a span with no ticks has not been timed at all.

The PWM tick times the runs, and the steady frames — the phase where the taken
interrupt *is* available — say what a tick is worth. That calibration returns
the master to 1.631 and the 68000 to 11,611 instructions a frame, both exact,
which is what says it is the same clock the rest of the project measured with.

Then six loops the scanner found on its own, three that read work RAM and three
that read a 32X register across the adapter:

| loop | charged | measured | reads |
|---|---|---|---|
| `880B1C` `move.l d0,(a0)` ×4 ; `dbf` | 11.60 | **11.59** | RAM |
| `8F4492` `move.w d2,(a1)` ; `dbf` | 9.00 | **9.04** | RAM |
| `8834C0` `tst.b ($ffffd1)` ; `beq` | 11.00 | **10.98** | RAM |
| `8818A4` `tst.w ($a15120)` ; `bne` | 13.00 | **12.97** | 32X |
| `881A36` `tst.w ($a15120)` ; `bne` | 13.00 | **12.98** | 32X |
| `881948` `tst.w ($a15120)` ; `bne` | 13.00 | **12.97** | 32X |

Nine cycles an instruction to thirteen, adapter and RAM alike, every one of them
within **0.4%** of what the 68000 manual charges — and the RAM loops are the
control that says the clock itself is sound. **A 68000 read of a 32X register
costs exactly what a read of anything else in that addressing mode costs.**
There is nothing to model and the carried debt that said otherwise is struck.

### The residue was ours: one flat number where the engine has two phases

The symptom was real even though the cause was not, and `--rate68k` prints it
directly — the 68000's own instructions a frame, which is the question
`sh2_insns` has always asked of the SH-2s:

| frame | `--interp` | `--recomp` | reference |
|---|---|---|---|
| 14-30, the comm poll | **9,834** at 13.00 c/i | 11,622 at 11.00 | 12.97 c/i |
| 33-40, steady | **11,619** at 11.00 | 11,622 at 11.00 | 11,611 at 11.00 |

Musashi is exactly right in both phases, to three figures, because it charges
each instruction what it costs. The recompiled build is flat in both, because
`recomp_cpi` is **one number** — and 11 is the right number for exactly one of
the two phases. It was calibrated on the steady state, where the reference's own
mix comes out at 11.00 cycles an instruction on the nose; through the boot, where
the engine sits in a 13-cycle poll of a 32X register, it runs the 68000 **18%
fast**.

That is the whole of the 68000 residue, it is in the build that is the default,
and the phase it is wrong in is precisely the phase "2.5 frames early" was
measured in. The interpreter was never early at all.

### The recompiled 68000 stops converting and starts counting

The fuel is cycles now, and there is no `recomp_cpi` at all.

`M68K_BLOCK(c, addr, n)` already spent `n` of the fuel per block; `n` was
instructions, which is the whole reason a conversion constant had to exist. It
takes a second argument now — what those instructions *cost* — and the fuel
becomes a cycle budget `cpu_run` hands across directly, the way it already
hands Musashi one.

The costs are Musashi's own `m68ki_cycles[0]`, dumped by `tools/m68kcycles.c`,
which links Musashi and nothing else — that is what keeps it out of the cycle
`build/m68k_recomp.c` is already in. m68kmake has expanded the table per
addressing mode, so a `tst.w (xxx).L` at 16 cycles and a `tst.w (a0)` at 8 are
different entries and no second table is needed here. Taking it from the
interpreter rather than from a reading of the manual is the argument
`src/m68k_testmem.c` makes about semantics, applied to timing: the oracle is
now measured right against the reference in both phases, and a table copied
from it agrees by construction instead of by proofreading.

What the table cannot hold is what the handlers add while running, and on a
68000 that is four things, all of them covered:

| | known when | charged where |
|---|---|---|
| `bcc` taken or not | run time | the edge, in `transfer()` |
| `dbcc` looping, expired, or never counted | run time | the edge, three prices |
| shift by an immediate, `movem` | build time | the block's sum |
| shift by a register | run time | where the count exists |

Two more are in Musashi and neither can run here: `mulu`/`muls` add two cycles a
set bit of the operand and `scc` two when it sets, and across the reference's
whole 68000 extract — **54,183 instructions — there is not one of either**.
89.3% of what it executes is pure table cost and the remaining 10.7% is the four
above. The fixed `USE_CYCLES(2)` and `(3)` in the generated opcodes turn out to
belong to `moves`, `cas` and `cas2`, which are 68010 and 68020 instructions.

**The gate, which was written before the work:** `--recomp --rate68k` matching
`--interp` frame for frame.

| frame | before | after | `--interp` | reference |
|---|---|---|---|---|
| 14-30, the comm poll | 11,622 at 11.00 | **9,834 at 13.00** | 9,834 at 13.00 | 12.97 c/i |
| 34-40, steady | 11,622 at 11.00 | **11,619 at 11.00** | 11,619 at 11.00 | 11,611 at 11.00 |

Over sixty frames the two builds now retire 644,195 instructions against
644,124 — **0.011% apart**, where the phase they disagreed in was 18%. The
steady state, which was already right and was the thing to watch, moved by one
instruction a frame.

That last 0.011% turned out to be one thing and it is now gone too. Taking a
vertical interrupt costs a 68000 **44 cycles** — the stacking and the vector
fetch — which Musashi charges out of its exception table and the recompiled
build charged not at all. One interrupt a frame at 11 cycles an instruction is
four instructions in 11,619, which is exactly what the steady state was over by.
It comes off `cpu_credit` rather than the fuel, because the fuel does not exist
between hand-overs: `m68k_run` assigns it the budget on the way in, so anything
spent out there is overwritten — which is what the first attempt at this did,
to no effect at all. Charged properly, **41 of 60 frames are now identical
between the two builds and the steady state matches exactly at 11,619** —
644,055 instructions against 644,124. What is left is a handful of early-boot
frames where the two take genuinely different paths, and one of them, frame 3,
is 86 of the 69. That is a difference in what was executed, not a cost the model
is missing.

One hazard came with it and is worth writing down, because it was invisible
until the fuel changed units. A block's cost is never allowed to be zero. The
table has no entry for an opcode that is not an instruction, so a block that is
nothing but an invalid word costs nothing — and such a block hands back *its own
address*, which the trampoline looks up and enters again. While the fuel was
instructions every entry spent one and the slice ended; spending cycles, a free
block is an infinite loop with no fuel between it and the frame. This cartridge
has two, and one of them, `0x0283C2`, is inside the address space the recompiled
build can be sent to.

The five diff gates: the four interpreted ones are untouched at 20,077 and
52,386 instructions and 194 and 1,037 blocks, and the recompiled 68000 holds
8,736 blocks agreed with 400 divergences against 398 — the same agreement, two
more rows, which is what a correct clock changing where the CPUs meet looks
like at this granularity.

### The slave's clock: two cycles in 1,047, and a frame that is not 60 Hz

The PWM timer is the only clock the slave has, and calibrating `refpoll.py`
against it turned up a 0.32% disagreement that nobody was looking for: the
reference ticks **367.14** times a frame, dead steady over sixty of them, and we
tick 365.96. Two causes, and the interesting part is that neither needed a clock
to find, because **a loop that closes on itself is one**. Its lap costs exactly
what its instructions cost — the branch is taken every lap, which is the entry
Musashi's table already holds — so counting laps against PWM interrupts says
what an interrupt is worth without anything being assumed. `--period` does that:

| loop | a lap | instructions | ticks | SH-2 cycles a tick |
|---|---|---|---|---|
| `8834C0` poll of work RAM | 2 for 22 | 415,630 | 13,116 | **1045.7** |
| `880B1C` long-word fill, `dbf` | 5 for 58 | 20,481 | 682 | **1045.1** |
| `881948` poll of a 32X register | 2 for 26 | 11,137 | 415 | **1046.6** |

Three quite different shapes agreeing on **1,046**, against the 1,048 we
computed. The period is `(cycle - 1) * tm` and we had `(cycle + 1) * tm` — two
cycles in 1,047, 0.19%. What settles it beyond the measurement is the register
itself: the game writes **0x417, which is 1,047, and 23,011,360 / 22,000 + 1 is
1,047 exactly** — the documented formula, for a driver aiming at a round 22 kHz.
Read our way it would have been aiming at 21,957.

One thing caught in passing, because it moved the `dbf` loop 3.4% off the other
two: a `dbcc`'s table entry is 12, which is *neither* of the prices it goes for.
Looping costs 10 and expiring 14. A `bcc`'s entry is its taken cost and needs no
adjustment; a `dbcc`'s needs one on every edge.

**The other 0.13% is the frame.** Turn the measured tick around — 348.60 68000
cycles apiece, 367.14 to a frame — and the reference's frame is **127,985
cycles**. NTSC says 128,005.7: the master clock is 53,693,175 Hz, a line is 3,420
of them and a frame 262 lines, so the 68000 gets 896,040/7. The two agree to
0.016%. Ours is `7.67 MHz / 60` = 127,840, and the error is almost all of it in
the **60** — NTSC runs at 59.9227 Hz.

Corrected, the PWM tick lands on 367.13 against the reference's 367.14 — and the
six loops of the section above, which came out **uniformly 0.2% under** what the
manual charges them, now read exactly: 11.00 for the poll of work RAM against
11.00 charged, 13.00 for the poll of a 32X register against 13.00, 11.61 against
11.60 for the fill. A systematic offset on six unrelated loops at once was a
clock and not six coincidences, and it was this one. It also moves the master to
**1.633** cycles an instruction from 1.631, which `sh2_cpi1000` still carries.

### The slave diff was reading its own aligner, not the slave

Both corrections made the slave diff *worse* — 1,037 blocks agreed, then 985 with
the period, then 900 with the frame — while the 68000's whole extract went the
other way, 449 divergences down to 413. One instrument disagreeing with three
measurements and a documented formula is a reason to distrust the instrument,
and this one had a known fault: it is the diff held to 2,000 reference blocks
because "the aligner cannot walk two streams of such different densities".

It could not, and the reason is one number. `holds()` accepts an alignment by
checking the next few reference lines turn up in order, each within **64** of the
last — and on the slave, whose stream is 93% delay loop, the reference collapses
each run into one line where we print every step of ours, so the next line sits
*thousands* of our records away. Every correct alignment across a collapsed run
was being rejected. Adding the run's own length to that reach is the whole fix:

```python
nxt = find(k + d, p + 1, exact, HOLD_REACH + ref[k + d].gap)
```

It stays a bound and never becomes a prediction, which is the distinction the
tool's own note insists on — `find` still takes the earliest match at or after
the current position, so a loop we spun fewer times is matched early. What
cannot work, and is still not done, is jumping N steps and looking there.

Every diff improved, including the ones that had nothing to do with the slave:

| | before today | after the clocks | after the aligner |
|---|---|---|---|
| 68000 boot | 20,077 / 20 | 20,076 / 23 | **20,083 / 21** |
| 68000 whole extract | 52,386 / 449 | 52,390 / 413 | **52,397 / 411** |
| master | 194 / 19 | 193 / 20 | **197 / 18** |
| slave, 2,000 blocks | 1,037 / 546 | 900 / 567 | **1,269 / 658** |
| recompiled 68000 | 8,736 / 400 | 8,736 / 400 | **8,741 / 397** |

And the bound went with it — to 20,000 reference blocks, walking the whole
extract with 17,885 agreeing, the agreement rate climbing with depth from 63% at
2,000 to 89% at 20,000.

*That last part did not survive the next section, and the fault was not the
aligner's.* The slave was banking its share of every hand-over while the adapter
still held it, and bursting through the backlog the moment it was handed the
cartridge — which carried the walk far past where our trace honestly reaches.
With the burst gone the reach is what the trace pays for and roughly linear in
it: 2,000,000 lines cross about 2,500 blocks and 8,000,000 about 7,000, so
20,000 would want some 21 million and a twelve-gigabyte file. The bound is back
at 2,000, which the budget covers with margin, and **lifting it is a trace-size
decision now rather than an aligner one** — which is still the thing that
changed. The aligner's own contribution stands: at that same 2,000 it takes the
walk from 900 blocks agreed to 1,269.

### The SH-2s were outrunning their own fuel, and the average was hiding it

The end-of-run report said the master ran **229,037** instructions a frame
against the reference's 235,076 and the slave 366,967 against 380,695 — 2.6% and
3.6% short, which reads like two CPUs idling where the reference works. It is
the wrong number. Take the *marginal* rate instead, the difference between a
60-frame run and a 120-frame one, and the sign flips: **239,661 and 383,800**,
1.95% and 0.82% *over*. An average over a run that starts with the SH-2s held in
the adapter's BIOS is not a rate.

Running fast has one cause and it is the defect `cpu_credit` was written to fix
for the 68000, never carried across. `sh2_run` checks its fuel at a block label
and then runs the block whole, so it returns more than it was asked for — and
the frame loop threw that difference away, 4,192 times a frame. The overshoot,
not the request, set the rate: 239,661 instructions a frame against the
**235,449 its own fuel allows**. A per-CPU credit, spent the way `cpu_run`
spends the 68000's, is the whole fix.

It was briefly too eager, and the giveaway was a number that looked *better* than
it had any right to: the rate came out identical over 60 frames and 120, to the
instruction. A CPU still held in the BIOS retires nothing, so its share was being
banked and then burst through the moment the cartridge was handed over — the
debt was quietly repaying the boot. A held CPU banks nothing now.

**That burst was also paying for the slave diff's 20,000-block bound**, and this
is the honest correction to the section above. It carried the walk far past where
our trace reaches on its own; with it gone the reach is what the trace pays for
and roughly linear in it — 2,000,000 lines cross about 2,500 reference blocks,
8,000,000 about 7,000. The bound is 2,000 again. The aligner fix is not what was
wrong: at 2,000 it still takes the walk from 900 blocks agreed to 1,269, and what
it changed is that the ceiling is now trace size rather than the walk stalling.

Then the constants themselves. `sh2_cpi1000` was 1.631 and 1.007, and both were
divided by the 127,840-cycle frame; against the real 128,006 the same reference
counts give **1.634 and 1.009**. Nothing about what the reference did moved, only
what we divided it by.

| | before | after | reference |
|---|---|---|---|
| master, steady | 239,661 (+1.95%) | **235,017 (−0.011%)** | 235,044 |
| slave, steady | 383,800 (+0.82%) | **380,593 (−0.034%)** | 380,724 |

**It did not move the 68000**, which is worth saying because the last section
guessed it would. Our 68000 still retires 11,634 instructions a frame against
the reference's 11,601 — our mix averaging 11.003 cycles an instruction against
11.034 — and correcting both SH-2s changed it by nothing at all. So it is not
downstream of them. Under `--interp` our cost model *is* Musashi's, and the six
loops `refpoll` prices land exactly on Musashi's charges, so the reference is
not costing instructions differently; what is left is that we execute a slightly
different mix. That is a question for the trace diff, whose 411 divergences on
the whole extract are mostly trip counts, and not for another clock.

### The 68000's last residue was a DMA nobody was charged for

Our frame retired 11,634 instructions against the reference's 11,611 and the
guesses about why had all been wrong, so the trace diff was asked instead — not
its divergence list, which is dominated by the boot, but one steady frame counted
per address on both sides. Getting that count right needs one thing said out
loud: the reference prints its `[Omitted: N]` for a collapsed run *after* the
vertical interrupt that broke into it, so a naive frame split throws away 11,238
of a frame's 11,611 instructions and leaves the handler looking like the whole
of it.

The answer is almost embarrassingly clean. Split the frame into the wait loop at
`0x8834C0` and everything else:

| | reference | ours | delta |
|---|---|---|---|
| the wait loop | 11,242.0 | 11,266.0 | **+24.0** |
| everything else | 369.5 | 367.8 | −1.7 |

**The work is the same work.** Excluding the loop, exactly two addresses differ
in the whole frame, by two instructions each. All 33 of the extra instructions
are extra *spins*, and the loop — `tst.b ($ffffd1)` / `beq`, 11 cycles an
instruction — is the cheapest thing in the frame, which is the entire mix
difference: our 11.003 cycles an instruction against 11.034 is not a cheaper
price for the same work, it is more of the cheapest work.

So the 68000 had spare time, about **264 cycles a frame**, and the trace says
where it went. The engine uploads its palette by DMA every vblank — 64 words to
CRAM, source `0xFFD460`, and the reference logs all 64 in every steady frame.
Our `vdp_dma` did the transfer and charged nothing. A DMA costs the 68000 the
bus for as long as it runs.

The rate is the VDP's own table, in transfers a scanline, and it is an order of
magnitude slower during active display than blanking because the VDP is fetching
pattern data:

| | H32 display | H32 blank | H40 display | H40 blank |
|---|---|---|---|---|
| → VRAM | 16 | 167 | 18 | 205 |
| → CRAM, VSRAM | 8 | 83 | 9 | 102 |

64 words to CRAM in H40 blanking is 0.63 of a scanline, **307 cycles**. Charged,
counted the same way on both sides:

| | reference | before | after |
|---|---|---|---|
| the wait loop | 11,242.0 | 11,266.0 | **11,238.3** |
| everything else | 369.5 | 367.8 | 367.7 |
| a frame | 11,611.5 | 11,633.8 | **11,606.0** |

From 22 instructions a frame over to 5 under — 0.19% to **0.05%**. The table
over-corrects slightly against the 264 cycles the frame measured, which is the
honest way round: 307 is what the VDP documents, and 264 is a difference between
two rates that each carry their own error bars.

### The 32X's own sound comes out, and it is silence

`--wav FILE` writes what the 32X played, `tools/diffpwm.py` holds it against
what the reference's 32X played, and over the whole 2.06 seconds of PWM output
the logs contain — 45,227 samples a channel — the two agree exactly. What those
samples are is the finding.

**The slave's driver is a four-channel PCM mixer and it was already running.**
The PWM interrupt enters at `0xC0000004`; every second one — a flip-flop in the
word at `0xC00001EC` — saves the register file, zeroes four accumulators and
calls `0xC000012C` four times. A channel reads a command word, takes a start
pointer, a length, a loop pointer and a rate out of a 16-byte table entry and
its two volumes out of the command's own high byte, then walks the sample data a
byte at a time against a fractional position: `xor #0x80` to sign it, `muls` by
the volume, four `shar`s, accumulate, and average with the previous sample —
which is where the second output of each pair comes from. The output stage then
converts to the unit's offset binary and pushes two stereo pairs, left to
`0x20004034` and right to `0x20004036`.

Three things about the unit come out of that code rather than out of a manual.

*The FIFOs are three words deep.* The driver's init writes exactly three words
to each, and having filled them never finds one full again: the branch at
`0xC00000DA` skips the wait at `0xC00000DC` in all 22,612 mixes the reference
logs. That only holds if the unit takes one sample out per PWM cycle against the
driver's two in every second cycle, which is what fixes the depth.

*A sample is a pulse width, so the cycle's own midpoint is silence.* The output
stage is `xor #0x400 / and #0x7FF / sub #0x200`, so the words run 0x000-0x3FF
about a centre of 0x200, where cycle 0x417 puts the midpoint at 523. The 2%
offset that leaves is what the coupling capacitor on the real output removes,
and what `dcblock` in `src/sound.c` removes here.

*The sample clock is not the interrupt clock.* It is one PWM cycle where the
interrupt is TM of them — the same thing only because this game programs TM 1,
which is why one accumulator in the frame loop had been enough. `src/sound.c`
owns both now, and all three CPUs retire the same instructions they did before.

**And there is nothing to hear.** Every one of the reference's 45,227 left
samples is 0x200 exactly. The 32X plays silence through the whole of the
opening, and so do we — which is what the gate is really checking today: that we
play nothing where the machine played nothing, since playing *something* is the
failure an ungated sound path produces.

**The sound in the opening is the Mega Drive's.** The Z80 runs 1,101,326
instructions over the same window, 646 distinct addresses all below `0x0F42`,
and writes the YM2612 through both of its port pairs — 95 register writes at
`$4000`/`$4001` and 77 at `$4002`/`$4003` — and the PSG 40 times at `$7F11`. It
takes 2,643 interrupts and writes the bank register at `$6000` 2,498 times, so
what it plays comes out of the cartridge. The 68000 drives the PSG directly too,
silencing all four channels at `0xC00010` during its own init. None of it is
modelled: `src/gen68k.c` still discards every write to the Z80's 8 KB and to
both chips.

*Gate met, for the half that exists.* `tools/diffpwm.py` derives the reference's
stream from the slave log itself — any instruction storing a word to one of the
three sample ports, with the register state the log prints before it executes
giving both the address and the value — so nothing in it knows the driver's
addresses, and it would still work if the game played a sample from somewhere
else. Ours comes from `--trace-pwm`, tagged with the CPU that wrote it, because
the 68000 pushes one zero through the mono port at `0x88072C` that no per-CPU
slave log can carry. `make check` runs it on the 300-frame trace it already
takes.

**The audio device is the frame clock now.** The loop had no limiter at all — it
rendered a frame and started the next — so the game ran at whatever speed the
host managed. With a device open it waits while more than a fifteenth of a
second is still queued: 600 frames is 10.013 seconds of game and takes 10.111
seconds of wall clock, with no underrun and no overrun. `--audio` opens a device
without a window, `--mute` refuses one with, and a headless run has neither,
which is what keeps `make check` at full speed.

### The fourth CPU

The Z80 runs, and `tools/diffz80.py` holds it to the reference the way the other
three are held: **32,719 of the reference's 33,984 logged instructions agree
exactly, with no fatal divergence anywhere in the extract.** Its 8 KB, the
68000's window into it, the bus request and reset that arbitrate between them,
and its own 32 KB window back into the cartridge are all in `src/genz80.c`;
`src/z80.c` is the CPU.

**The gate holds the upload as well as the core.** The Z80's program is in the
cartridge in no form a static tool could find — the 68000 assembles it into RAM
at run time — so there is nothing to disassemble ahead of time and a byte wrong
in the copy is a wrong instruction. The reference settles what should be there
by executing it: the first thing its Z80 runs after the console reset is a stub
left over from before it, `di / im 1 / jp 0x005B`, and the real driver appears
after the 68000 has loaded it and pulsed reset, restarting at 0x0000 with
`xor a / ld bc,0x1FD9` — the SMPS clear-and-go. Ours has no leftover, so the
stub and its 65,536-iteration delay are 22 reference instructions we cannot run;
from the driver's own first instruction the two streams are the same sequence.

**The core is written the way the instruction set is laid out**, as the grid of
`x = op >> 6`, `y = (op >> 3) & 7`, `z = op & 7` that it actually is, so a
missing instruction is a missing line rather than a gap in a list of 256 cases.
Two things in it are load-bearing rather than pedantry. The undocumented flag
bits X and Y carry bits 3 and 5 of whatever the last operation produced, and the
reference prints AF — without them nearly every logged line would differ and the
comparison would be worth nothing. And DD and FD do not simply mean IX and IY:
they replace HL, and they replace `(hl)` with `(ix+d)`, but never both in one
instruction — where the instruction already names memory, H and L stay
themselves.

Two things were wrong, and the trace said so both times.

*The refresh counter was counting operand bytes.* R advances once per opcode
fetch, not once per byte, and a prefix is a fetch of its own. It is not
bookkeeping here: the driver reads it at 0x0876 as its source of randomness and
stores the result, so a counter that counts the wrong thing goes straight into
what the sound does.

*Every prefix was charged twice.* `cyc_main` holds four cycles for the DD, FD,
ED and CB bytes and the code that dispatched them added four more, and then each
prefixed form added its full documented cost on top of the base it had already
paid — so `bit 7,(ix+d)`, the driver's second most frequent instruction, cost 31
cycles against the real 20. The measurement that found it is the one the clock
should be held to anyway: **the reference retires 9,528 Z80 instructions between
two vertical interrupts, steady to within five over the whole extract.** We ran
9,477. Corrected, 9,549 — 0.22% over, which is what remains.

What is left divides into three, and none of it is a wrong answer:

| rows | where | what |
|---|---|---|
| 102 | `0x0878` | `ld a,r`, one per frame — a randomness source whose absolute value carries history from before the reference's reset |
| 165 | `0x0038`, `0x009F`-`0x00A2` | trip counts in the idle loop and between interrupts, which is the 0.22% |
| 60 | `0x0F42` | one bit of a per-frame counter at 0x1BF7, decremented once a frame from a start we reach at a different phase |

Its traffic is now visible too: over 300 frames the driver makes 44 register
writes to the YM2612's first half and 36 to its second, and 8 to the PSG, which
`src/sound.c` latches and counts against the reference's own 95, 77 and 40. The
68000's four PSG writes — attenuation 15 on all four channels, its own init
silencing them — arrive there too. Modelling the Z80 also took the 68000's
unmapped accesses from 276 a run to 1, and every other number in the run is
unchanged to the instruction.

### The PSG plays, and so does the PWM once the logo is over

`src/psg.c` is the SN76489 in the VDP: three tone channels counting down from a
10-bit period, a noise channel shifting a 16-bit register, a 4-bit attenuator
each, and one write port where a byte with bit 7 set latches a register and one
without extends it. It mixes into the same output the PWM does, because on a 32X
that is where the Mega Drive's audio goes.

**Neither the trace nor this game can check it, so two other things do.** The
driver attenuates all four channels to 15 in its init and never lifts one in
anything the reference logs contain — 4 volume writes and 36 tone writes across
the extract, a music track playing with its own chip muted — so the reference
says nothing whatever about whether the core works.

What can be checked is the chip's own arithmetic, and `tools/test_psg.py`
computes each expectation rather than copying it out of the implementation: a
tone channel's frequency is clock / (32 × period), tested at 1, 285 and 1023; an
attenuation step is a factor of 10^-0.1; periodic noise repeats every sixteen
shifts and white noise does not repeat in four thousand; the three fixed noise
rates are the clock over 512, 1024 and 2048 and the fourth is channel 2's
period; and a period of zero parks the channel rather than oscillating at half
the counter clock. Eighteen cases.

And the *input* can be checked exactly. `tools/diffz80.py` now ends by comparing
the bytes that reached the two chips against the bytes that reached the
reference's — every store to `$4000`-`$4003` or `$7F11` in the log, with the
register state before it saying what the byte was. **All 164 agree, in order**,
plus four leading PSG bytes of ours that are the 68000's own and that no per-CPU
log can carry. The instruction diff says the driver runs right; this says what
it produced.

**Past the SEGA logo there is music, and both halves of the machine are making
it.** The earlier section's "there is nothing to hear" was true of the reference
extract, which is 1.7 seconds; run 1,800 frames and the picture is different.
The PSG has three channels at attenuations 8, 5 and 7, and `--sound`, which
separates the two sources the way `--layers` separates the video's, says what
each is worth:

| | peak | RMS over the second half |
|---|---|---|
| `--sound 1`, the 32X's PWM | 32,767 | 2,562 |
| `--sound 2`, the Mega Drive's PSG | 3,638 | 816 |
| both | 32,767 | 2,687 |

The PWM is not silent either: 659,860 samples over those frames, changing value
461,657 times where the whole reference extract was one constant. And the PSG's
output is music rather than merely non-zero — a Goertzel over the isolated
stream finds A#3 and F4 together at twenty seconds and C4 and G4 at
twenty-five, each with the octave-and-fifth harmonics a square wave has, and
those fundamentals are the tone periods the driver wrote.

What is chosen rather than measured is the balance: four PSG channels at full
volume come to half of the output's range, against the PWM's full swing. What a
Mega Drive's PSG is worth against a 32X's PWM is an analogue question the traces
cannot answer.

### The FM, and the whole machine making one sound

Nuked-OPN2 is the YM2612, cloned into `third_party/` the way Musashi is and
ignored by git the same way, so nothing of it enters the repository. The reason
is the problem the PSG's section had to work around: **a sound chip has no trace
oracle.** The reference logs instructions and not audio, so a hand-written OPN2
could have been held to nothing, where a die-derived one is what every other
implementation is measured against. Its register input is gated regardless —
`tools/diffz80.py` already holds the 164 bytes that reach the two chips to the
reference's own.

What a wrapper around it has to get right is the clock and the write latch, and
both are checkable.

*The clock is the 68000's, divided by 144*, so a frame is 888.93 samples. Over
1,800 frames the chip produces **1,600,075, which is 128,006 / 144 × 1,800 to
the sample.**

*Nuked latches a write on its next clock and keeps one pending value for the
address and data ports together*, so an address byte followed by a data byte
with no clock in between loses the address — and that pair is exactly what the
driver writes, a few Z80 instructions apart and well inside one hand-over here.
Clocking once after each write is what makes the pair safe, and the cycle it
spends early is taken back out of the next tick's budget so the chip still runs
at its own rate.

**All three sources play, and they are playing the same piece.** At 1,800 frames
— thirty seconds, well past the logo — `--sound` separates them:

| | peak | RMS | stereo |
|---|---|---|---|
| `1` the 32X's PWM | 32,767 | 2,562 | 160 |
| `2` the PSG | 3,638 | 816 | 0 |
| `4` the YM2612 | 7,414 | 1,642 | 691 |
| `7` all of it | 32,335 | 3,162 | 734 |

The FM is the only stereo source, which is what its per-channel L/R enables are
for; the PSG is mono by construction and the PWM's two channels carry nearly the
same thing. Nothing clips. And a Goertzel over each isolated stream says they
agree with each other: at twenty seconds the FM is on C3 with its C2, C4 and C5
harmonics while the PSG holds C4 and G4, and at twenty-five they move together
to D and G.

Two things this game does not do. It never enables **channel 6's DAC** — zero
writes to 0x2A over those 1,800 frames — so its drum samples are not coming
through the FM. And it writes nothing audible to any of the three during the
SEGA logo, which is why the first two sections of this milestone both ended in
silence: the PSG muted, the PWM a constant 0x200, and the YM2612 taking 80
register writes that are a voice being set up rather than played.

The remaining chosen-not-measured number is the mix. The FM is scaled by six
against the PWM's full swing and the PSG's half, which puts a loud FM channel
where a loud PSG channel is. What the three are worth against each other on real
hardware is an analogue question and the traces do not answer it.

### An interrupt lands where the clock puts it, not where the hand-over does

Both SH-2s were being told about an interrupt at the start of a window in which
it had not yet happened, and the two cases are the same defect seen from
different sides: an event's *position inside a hand-over* was being thrown away.

**The slave's timer was firing early by construction.** The PWM interrupt is the
only clock it has, and the frame loop raised it for a whole hand-over at that
hand-over's start — up to 90 SH-2 cycles ahead of the timer's own edge on a
period of 1,046, a twelfth of the clock, and then the driver began mixing from a
point the timer had not reached. `sound_pwm_ahead()` says how much of the next
share falls before the edge, and the slave's slice is cut there: run to the
edge, take the sample out of the FIFOs, raise the interrupt, run the rest.

**The master was answering the 68000 before the 68000 had asked.** Inside a
hand-over the 68000 runs its whole share and the SH-2s run theirs afterwards,
both standing for the same wall-clock window — so a register write in the last
cycles of the 68000's turn was acted on from the *start* of the SH-2's, a reply
that precedes the question by up to a whole hand-over.

The game measures exactly that, and the measurement is a count the trace already
holds. At `0x883232` the 68000 arms a DREQ transfer, raises the master's command
interrupt, and spins at `0x88323A` on the bit until the handler clears it —
which takes the master eighteen instructions, twelve of the dispatcher at
`0x060001B0` and six more to the `mov.w r0,@r0` at `0x0600133E`. So how many
times the 68000 goes round is how long the master took. The reference goes round
twice in 45 of its 103 transfers, three times in 26 and once in 31, mean
**1.97**. Ours went round *once* in 194 of 270, mean 1.28: whenever the write
landed near the end of a hand-over, the master answered before the 68000
executed another instruction at all.

So a raise now carries where in the window it happened — `mars_slice_pos()`
turns the 68000's unspent cycles into the SH-2's own — and the target's slice is
split there, the same cut the PWM edge makes.

**Measuring from what is *left* rather than what has been spent is what makes
that a position at all.** The budget is not the window: a slice that overshot
leaves a debt and a DMA that held the bus takes more off, so `cpu_credit` is
short by exactly how far into the window the 68000 starts. And the answer is
allowed to land past the window's end, carried into the next hand-over rather
than clamped — which is the two backends being honest about themselves.
Musashi charges an instruction when it finishes, so it places a write within one
instruction of the truth; a recompiled block is charged whole in its prologue,
so it places every access inside the block at the block's *end*. For the block
that matters here that is five hand-overs out, and costs nothing, because
`0x883202` is fifteen instructions and 154 cycles and the raising write is its
last one.

| | before | after |
|---|---|---|
| 68000 boot | 20,083 agreed, 21 div | **20,084**, **19** |
| 68000 whole extract | 52,391 agreed, 421 div | **52,414**, **389** |
| recompiled 68000, `--blocks` | 8,741 blocks, 395 div | 8,741, **384** |
| slave, 2,000 blocks | 1,131 agreed, 482 div | **1,344**, 593 |
| master, boot | 197 blocks, 18 div | unchanged |

The picture is unchanged to the byte — 15,116 frame-buffer bytes, 224 of 224
line-table entries, 362 palette bytes, 249 commands — and so is the sound: the
same 659,860 samples over 1,800 frames at the same peak, and 3,170 RMS against
3,162. Every byte reaching the two Mega Drive chips is still the reference's.

The slave's divergence count rising while its agreement rises by a fifth is the
same thing that happened when it stopped being unwound after 64 idle reads:
more of it is being compared. 1,344 of 2,000 reference blocks in lock step is
67%, where it was 57%.

**What is left of the command rendezvous is a cycle model, not a schedule.**
Ours now polls twice in 121 of 270 transfers, mean **1.57** against the
reference's 1.97. The poll loop is a `btst` whose read lands about 16 cycles in
and a `bne`, 30 cycles a lap, so the counts price the ack directly: the
reference's is about 45 68000 cycles after the raising write, or 135 of the
master's, where ours is 29 — eighteen instructions at `sh2_cpi1000`'s flat 1.634
apiece. That constant is a frame average dominated by cache-resident tight
loops, and this path is nearly all memory: PC-relative loads out of SDRAM and
writes into the 32X register block. A short run of exactly the wrong kind of
instruction is charged a quarter of what it costs.

The third of a poll left over is ten 68000 cycles, and two things we cannot
separate from it are that size or larger: the SH-2's own exception entry, which
we charge nothing for, and the reference emulator's scheduling quantum — the
interleaved log shows it handing the master 44 instructions in one uninterrupted
run, and its own poll counts spread over 1 to 4.

The same debt is what the largest remaining 68000 group is made of. 104 rows at
`0x88314C`, `0x8831B4`, `0x8831B6` and `0x8836E6` are all one fact: our master
finishes a command before the next vertical interrupt where the reference's is
still busy, so the engine's vblank handler drains the palette queue at
`0xFFD860` every frame instead of skipping it. At the divergence the reference's
queue holds 32 entries and ours holds one.

### Past the logo the game stopped moving, and the register was eight bits wide

Asked whether it is playable, the honest answer was no, and the way it fails is
worth more than the answer. It boots, draws the SEGA screen correctly at frame
300, and plays the music — and then everything freezes. VRAM, CRAM, the 32X
frame buffer and the command counts are **byte-identical at frames 600, 1,200,
3,600 and 9,000**, which is two and a half minutes of game time; `--hold start`
changes nothing. Speed is not the constraint: a headless run does 1,800 frames
in 2.4 seconds, twelve times real time.

Where it stopped said what was wrong. The 68000 parks at `0x8845CE`, the comm-0
acknowledgement wait, and the master parks in a FEN spin at `0x060047E6` —
inside the 32X VDP's autofill blitter at `0x06004750`: wait for FEN, write a
length to `0x20004104`, a start to `0x4106`, data to `0x4108`. Counting what
that costs is one line of instrumentation and it is unambiguous:

| | 300 frames | 1,200 | 3,600 |
|---|---|---|---|
| longest fill | 256 words | **65,534** | **65,534** |
| words a frame | 31,145 | 132,925 | 172,231 |
| of the master's cycles | 16% | 69% | **90%** |

**The auto fill length register has eight bits and we were taking sixteen.** The
blitter hands it a count that can arrive as `0xFFFE`, which is 254 words on the
machine and 65,534 here — the same 256 words written over and over, because
`autofill()` already increments only the low eight bits of the address, but
charged 256 times the FEN. The master waited for every one of them.

The reference settles it from the other side without needing to reach the
blitter at all. Its own clear loop at `0x06003180` writes **`0xFF`**, the
maximum an eight-bit field holds, and steps the start address by `0x100`,
256 times, to cover the 128 KB frame buffer exactly. A length wider than the
block a fill wraps inside cannot mean anything.

**Masked, the game starts moving again.** Commands go from frozen at 309 to 999
by frame 5,400, and the 68000 leaves `0x8845CE`. Every gate is untouched — the
reference extract is 1.7 seconds and never asks for a fill longer than 256
words, so the 300-frame run this is all measured on is identical to the byte.

**It still does not reach the title screen**, and there are now two named
reasons rather than a freeze.

*The master is fill-bound.* 133,224 words a frame is the whole frame buffer
cleared twice over, 69% of its cycles, and what it draws into the buffer
afterwards is 255 bytes — it clears and clears again. `--frames N` now reports
the fill count, the words and that percentage, because a fill that is wrong by
256x has no other symptom than a game that stops.

*The Mega Drive picture is parked one screen below the display.* Both planes
hold 16 rows of real content — 40 cells wide on A, 62 on B, with a 38-entry
palette — and VSRAM holds `0xFF20`, a vertical scroll of exactly −224 lines,
which is the screen's own height. So the renderer is not drawing nothing; the
engine has a screen ready to slide in and never slides it. Chased further in the
carried debts below: the picture is the title screen's tunnel and is correct,
the engine writes that one scroll value 2,800 times without ever changing it,
and neither the recompiler nor the fill cost is what holds it there.

**And there is 68000 code past the logo that discovery has never seen.** The
recompiled build hands over to the interpreter 4,163 times at
`0x0749C2`-`0x0749D0`, which is not a block and is not in `az.code` at all,
sitting between known blocks at `0x749AA` and `0x749E4`. `coverage` is clean
because it is asked only of traces that stop at 1.7 seconds.

That is the shape of everything above. Five trace diffs, two byte-exact
round-trips and a sample-for-sample audio gate, all of them ending 1.7 seconds
in — and the first thing past that end was a register width.

### Next

**1. The SH-2s keep time with one number each.** `sh2_cpi1000` is 1.634 and
1.009 cycles an instruction, measured over whole reference frames — and the
68000 has already been through this: `recomp_cpi` was 11, exactly right in the
steady state and 18% fast through the boot, and the fix was for each block to
carry what its own instructions cost. The SH-2 side is the same shape and
harder, because there is no Musashi to take a table from and because an SH-2's
cost is mostly its cache. It is what the two largest remaining 68000 groups are
made of — 114 rows of command-interrupt latency and 104 of a master that
finishes too early — and it is now the thing in front.

**2. The sound has no accuracy gate, only an input gate.** Every byte that
reaches the two chips is the reference's, and what comes out of them is
unmeasured against anything: the reference logs instructions and not audio.
Nuked-OPN2 removes the question for the FM, and the PSG has its arithmetic
test, but the *mix* — three sources at chosen relative levels — is a judgement
and would stay one even with a better oracle.

**3. Everything is gated on 1.7 seconds.** Every diff, and therefore every
claim of accuracy, stops where the reference logs stop — and the game stops
working shortly after that. The fill-length bug is what that costs: a
hardware-register width wrong in a way no gate could see. Three threads lead out
of it, in order of how much they would buy: why the 68000 issues command 7 three
times in 5,400 frames when the asset table it fills is what the master draws
from; why the Mega Drive screen sits parked at a scroll of -224; and the
`0x0749C2` region the front end has never discovered. None of them has an
oracle, which is the point — past 1.7 seconds the only instrument is the run
report, so anything found there should leave a number behind in it.

**4. The slave diff's bound.** Still 2,000 reference blocks, but for a different
reason than before: the aligner walks collapsed runs now, and what limits the
bound is how far our own trace reaches. It is roughly linear in trace size —
2,000,000 lines cross about 2,500 blocks, 8,000,000 about 7,000 — so raising
`--trace-sh2-lines` and the bound together is the lift, and it costs disk rather
than cleverness. 20,000 would want some 21 million lines.

### Not worth doing, and measured rather than assumed

The point of this list is that each line cost an experiment, so the next session
does not spend the same day finding the same thing.

* **A wait-state penalty for crossing the adapter.** There is none to charge.
  Six loops on the PWM clock, three reading work RAM and three reading a 32X
  register, all within 0.4% of what the 68000 manual charges — the section above
  has the table. Charging anything at all would push the steady state off, where
  we run 11,619 instructions a frame against the reference's 11,611.
* **Timing anything in the boot phase by the master's instruction count.** It is
  the clock that invented the penalty. The master runs at 1.631 cycles an
  instruction in the steady state and 1.30 through the comm poll, and using the
  first to time the second inflates every figure by 25%. `tools/refpoll.py`
  reports both clocks side by side for exactly this reason; where they disagree,
  the PWM tick is the one with hardware behind it.
* **The VDP's own vblank marker as a frame clock.** It is in the log, it is
  independent of whether the CPU takes the interrupt, and it is not a clock: 72
  of them arrive in bursts with no instructions in between.
* **The 68000's taken vertical interrupt as a boot clock.** Exactly 127,840
  cycles apart and unimprovable in the steady state, which is the phase that
  never needed it. The engine boots with interrupts masked and 937,053
  instructions go by without one.
* **Charging the recompiled 68000's interrupt against `m68k_fuel`.** It changes
  nothing, silently: `m68k_run` assigns the fuel its budget on the way in, so a
  cycle spent between hand-overs is overwritten before it is read. The account
  that survives a hand-over is `cpu_credit`.
* **Reading a `dbcc`'s cycle-table entry as what it costs.** It is 12, and a
  `dbcc` goes for 10 when it loops or 14 when it expires — never 12 unless the
  condition was true. A `bcc`'s entry *is* its taken cost, which is what makes
  the trap look safe. It put a `dbf` loop 3.4% off two polls that agreed.
* **Reading a CPU's rate off the end-of-run average.** The SH-2s are held in the
  adapter's BIOS through the early boot and retire nothing there, so a 60-frame
  average reads 2.6% *under* a steady rate that is really 1.95% *over*. Take the
  marginal rate between two run lengths. The same average is what made a credit
  that banked the hold look like an improvement.
* **Splitting the reference into frames at the vertical interrupt marker.** The
  collapsed run the interrupt broke into is printed *after* the marker, so a
  split that resets its history there loses it — 11,238 of a steady frame's
  11,611 instructions, leaving the handler looking like the whole frame and its
  373 instructions looking like the rate.
* **Reading the slave diff's reach as an aligner result.** It is a trace-size
  result. A bound that walks further because the slave burst through a banked
  backlog is not coverage of anything, and it is what made the aligner fix look
  like a ten-fold lift when the honest figure was 2,500 blocks for 2,000,000
  traced lines.
* **Finer hand-overs.** 32 and 64 sub-slices to the line are both worse than 16.
  Neither is the lever anyway: what the granularity was costing was an event's
  *position* inside the window, and carrying that explicitly is free where
  halving the window is not.
* **Reordering the CPUs inside a hand-over.** It brackets the answer and hits
  neither side. Carrying the raise's position does what reordering was reaching
  for, without having to pick a side.
* **Reproducing the reference's interrupt *landing points*.** It defers the
  slave's PWM interrupt to one point in a 206-instruction loop, 3,360 times out
  of 3,360, which is idle-loop handling in the emulator rather than a fact about
  the machine.
* **A VDP bus-contention model** — the first attempt at the 68000's clock. It
  changed the instruction count by nothing at all, which is what said the budget
  was not what governed. See the note above `cpu_credit` in `src/mars_main.c`.

### Carried debts

* ~~**The asset table is barely filled.**~~ *Settled.* The extended extract
  shows the reference issuing command 7 exactly once across 5,643,099 master
  instructions, and our 300-frame run posts exactly one. A mostly-empty asset
  table is what this point in the game looks like. The 10.8 million reads of
  address 0 are the sprite drawer walking slots that are legitimately unfilled.
* ~~**96 of the 224 lines have no line-table entry.**~~ *Settled — it was a bug,
  not a property.* The autofill start register was being read as a byte address
  where it holds a word address, so every fill landed at half its address and
  zeroed the line table the master had just written. 224 of 224 now.
* ~~**The 68000 does not pay to cross the adapter.**~~ *Settled, and there was
  nothing to pay.* The 16.3 cycles an instruction that said otherwise was 12.97
  timed with the master's steady-state rate through a window where the master
  runs at 1.30. On the PWM clock a 32X register read costs the 68000 exactly
  what the manual charges, across six loops and both kinds of memory.
* ~~**The recompiled 68000 keeps time with one number.**~~ *Settled.* There is
  no number: each block carries what its own instructions cost, summed from
  Musashi's table at build time, and the fuel is cycles. The two builds now
  retire the same instructions to 0.011% over sixty frames where the boot phase
  was 18% apart.
* ~~**The slave's PWM tick is 0.34% slow.**~~ *Chased, and it was two things.*
  The period is `(cycle - 1) * tm`, not `(cycle + 1)` — 1,046 SH-2 cycles rather
  than 1,048, measured off three closed loops and confirmed by the register the
  game writes being exactly what the documented formula gives for 22 kHz. The
  rest is the frame below.
* ~~**The frame is 60 Hz where NTSC is 59.9227.**~~ *Settled.* 128,006 now, the
  true 896,040/7, confirmed against the reference's own clock at 127,985.
* ~~**The 68000's steady state is 11,634 a frame against the reference's
  11,601.**~~ *Settled, and it was the DMA.* All 33 instructions were extra
  spins of the cheapest loop in the frame, bought with the 264 cycles a palette
  DMA should have cost the 68000 and did not. 11,606 now.
* ~~**`sh2_cpi1000` was measured against the short frame.**~~ *Settled.* 1.634
  and 1.009 now, from the same reference counts over the right frame.
* ~~**Interrupt phase — the hand-over quantises both timers.**~~ *Settled.*
  Neither is quantised to the hand-over now: the slave's slice is cut at the PWM
  timer's own edge, and an interrupt the 68000 raises carries the position in
  the window it was raised at and cuts the target's slice there. What was left
  after that is a cycle model rather than a schedule — see the section above.
* **The vertical interrupt's position inside line 224 is not measured.** We
  raise it at the top of the line and the 68000 takes it at the first
  instruction boundary, which is what the reference's own markers look like:
  102 of them, all `@ 224,n` with n between 3 and 8 of the ~211 hpos units in a
  line, or 7 to 18 cycles in. Never 0 to 2, which hints the raise itself is a
  unit or two past the line's start rather than on it — but that is a couple of
  cycles inside a 22-cycle wait loop, and it cannot be told apart from where the
  reference emulator happened to have an instruction boundary. 38 rows at
  `0x8834C0`/`0x8834C4`, down from 58, are this loop's phase.
* ~~**No audio.**~~ *Settled.* All three sources play and every byte reaching
  them is the reference's: the 32X's PWM gated sample for sample, the Z80 gated
  instruction by instruction, the PSG tested against its own arithmetic, and
  the YM2612 supplied by Nuked-OPN2.
* **The mix balance is chosen, not measured.** The PWM has the output's full
  swing, four PSG channels at full volume half of it, and the FM is scaled by
  six; at 1,800 frames the three measure 2,562, 816 and 1,642 RMS. What they are
  worth against each other on real hardware is an analogue question, and nothing
  in the traces answers it.
* **The Z80's clock is 0.22% fast.** The reference retires 9,528 instructions
  between two vertical interrupts and we retire 9,549. Every cycle count in
  `cyc_main` and every prefixed form has been checked against the manual once;
  finding the last 130 cycles a frame wants the per-address frame comparison
  `tools/refframe.py` does for the 68000, not another reading.
* **One PWM sample is lost at boot.** The 68000's 32X init pushes a zero through
  the mono port at `0x88072C`, which is one word in each three-deep FIFO before
  the slave's driver fills them with three more, so the driver's last write is
  dropped. Whether hardware clears the FIFOs when the cycle register is zeroed
  is not something the logs can say, and it is one sample of silence either way.
* **Genesis DMA fill and copy are skipped.** The reference issues 65,535 fills
  during boot; `vdp_dma` returns early on both — and now returns early *before*
  charging the 68000 for them too, so a fill is free where a transfer is not.
  The transfers are the ones the engine uses every frame; the fills are the
  boot's.
* ~~**The 32X frame-select polarity is a guess.**~~ *Settled, once there was a
  correct picture to look at.* The buffer we display holds the whole SEGA logo
  and the one the CPUs draw into holds a partial redraw of its bottom edge —
  reversed, the display would be the torn half.
* ~~**The palette conversion is unchecked.**~~ *Settled, and it was right.* Red
  is the low five bits and blue the high five: read that way the logo is blue
  and the nebula magenta, read the other way it is the red thing this drew
  before the bank register was fixed. What the trace could not reach directly —
  the reference's CRAM writes are memory-to-memory inside elided loops — it
  reached through the *source*: 38 of 38 of the palette words the reference
  loads come from the cartridge at 0x200000.
* **The picture is right, not merely recognisable.** At 300 frames the 32X half
  draws the SEGA logo in blue with its outline, its "TM" and the nebula panels,
  and the line table matches the reference's own arithmetic.
* **The master is fill-bound past the logo.** 133,224 words a frame through the
  32X VDP's autofill, which at the two SH-2 cycles a word the reference measures
  is 69% of its clock, and what reaches the frame buffer afterwards is 255
  bytes. Whether a real 32X is that fill-bound is not something the extract can
  say — it never reaches the blitter at `0x06004750`.
* **The Mega Drive screen past the logo is parked below the display, and the
  engine parks it.** Rendered with the vertical scroll forced to zero it is the
  Chaotix title screen's perspective tunnel, drawn correctly — so the tiles, the
  tilemap, the palette and the renderer are all sound. What holds it off-screen
  is the engine writing VSRAM 0 and 1 as `0xFF20`, exactly -224 lines and
  exactly the screen's own height, from frame 422 onward: 2,800 writes over
  3,600 frames and **one distinct value**. It is not a DMA being dropped either
  — there is no transfer to VSRAM in the whole run, every write is through the
  data port.
* **It is not the recompiler.** `--interp` and `--recomp` are identical past the
  logo: the same 359 commands at 1,200 frames and 492 at 3,600, the same 9,583
  VRAM bytes, the same 38 CRAM entries, the same frame buffer. The only
  difference is the reported PC, which is the same instruction seen through the
  direct and `0x880000` windows.
* **The fill cost throttles the engine but is not what stops it.** Making
  autofills instantaneous takes the command rate from 492 to 1,291 in 3,600
  frames, 2.6x — and the scroll is still that one value and the screen is still
  black.
* **The level picture recorded under M5 was a much faster machine, not a
  regression.** Bisected at 3,000 frames: `a5b2522` and `bd06bcc` post 1,751
  commands where the current build posts about 400, because the SH-2 rates, the
  FEN model and the interleaving were all still wrong in the direction of
  running the game too fast. None of those commits draws a Mega Drive picture at
  3,000 frames either.
* **`0x0749C2`-`0x0749D0` is 68000 code discovery has never found.** Not a
  block, not in `az.code`, between known blocks at `0x749AA` and `0x749E4`, and
  entered 4,163 times in 3,600 frames. `coverage` is clean because it is only
  ever asked of traces that end at 1.7 seconds.
* **Genesis VDP gaps:** no window plane, shadow/highlight, interlace, or the
  per-line sprite and pixel limits.

**M6 — sound** ✅ *done — the game plays its music*

Every source is in and every input to them is gated. The 32X's PWM is modelled
down to its three-word FIFOs and `tools/diffpwm.py` holds what it plays to what
the reference played, sample for sample. The Z80 runs the driver the 68000
uploads, gated at 32,719 of 33,984 instructions, and the 164 bytes it puts into
the two Mega Drive chips are the reference's own, in order. The PSG is ours,
with an arithmetic test of its own; the YM2612 is Nuked-OPN2. An SDL device
paces the run at the machine's speed and `--sound` separates the three.

What is not gated is the *sound itself*, because the reference logs instructions
and not audio — see the note under Next. The other half of this milestone,
replacing the 68000 interpreter with recompiled code, was done earlier and is
the default; what remains of it is optimisation, and the interpreter stays for
the code the engine writes at run time.

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
