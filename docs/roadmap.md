# Roadmap

What is left to do, and what has already been measured about doing it.

[done.md](done.md) is how the project got here — the milestones, and the findings
and the wrong turns behind them. [state.md](state.md) is what is true of the
build right now, facts only.

## The plan

**Static recompilation**, the N64Recomp model: mechanically translate the
original machine code into C, compile that with clang for arm64, and supply a
native runtime for everything that was hardware. The output is a native ARM
binary. That is what "recompile it to ARM" describes and it is what this is.

Not **matching decompilation**, the sonic-disasm / sm64 model, where hand-written
C is fed to a period-correct compiler to reproduce the cartridge byte for byte.
That output is source rather than a program you run; for a 3 MB two-architecture
title it is a multi-year effort, and the 68000 half has no usable matching
compiler story. The two share their entire front end — decoder, code discovery,
control-flow recovery — so nothing built here is wasted if the goal ever shifts.

What it came to in practice is what the framing predicted: **a purpose-built
high-level 32X runtime with the CPU cores replaced by recompiled native code**,
and a fair amount of hardware emulation written regardless of how good the
recompiler got. Every part of the machine this game touches now exists — both
SH-2s and the 68000 recompiled, the Z80 interpreted, both VDPs, all three sound
sources, the six-button pads and the cartridge battery. [state.md](state.md) says
what runs what.

## Milestones

| | | |
|---|---|---|
| M0 | foundation | ✅ |
| M1 | SH-2 front end | ✅ |
| M2 | 68000 front end | ✅ |
| M3 | SH-2 → C recompiler | ✅ |
| M4 | runtime skeleton | ✅ |
| M5 | first pixels | ✅ |
| M6 | sound | ✅ |
| M7 | **it plays** | 🔵 in progress |

M0–M6, and what closed each of them, are in [done.md](done.md). There is no M8:
nothing after "it plays" is blocked on a part of the machine that does not exist.
What follows M7 is accuracy, coverage and speed — the debts at the bottom of this
file.

## M7 — it plays

`./build/mars` boots, draws the SEGA screen, assembles the title screen and runs
the attract loop for 200,000 frames — an hour and a half of game — with no
unmapped access, no parked CPU and no stall in the command rate. Played from the
keyboard it goes further than any headless run reaches: the save select and
player select render, a level plays and can be completed, the special stage draws
its tunnel, its HUD and the molecule background and is playable to its Chaos
Ring, and the Newtrogic High Zone hub, the Combi Catcher and the attraction
select all draw.

What the milestone still owes is not more emulation. It is that **every fault
found since the SEGA logo was found by a person playing the game** — thirteen
missing blocks, an eight-bit register taken for sixteen, a divide unit that was
plain storage, a banked window folded to bank 0, two delay slots — and
`make check` was byte-identical across every one of them.

That is no longer quite true, which is the first movement on this in the
project's history: `--xcheck` found a wrong CCR immediate that nobody had seen
and nothing could have seen. One gate, one fault, and it is the 68000 only —
but the sentence above was the whole of what M7 owed, and it now has an
exception. The reference logs are
1.7 seconds; the game runs for two hundred and fifty.

The ordering below is by what each would have caught, not by how interesting it
is.

### 1. Gates that do not need the reference

*Partly delivered.* `tools/test_session.py` replays a recorded play session as
the last step of `make check` and fails a build on the conditions that are always
a bug however the runtime changes: a transfer with no recompiled block, a CPU
parked at zero, an unmapped SH-2 access, the movie not reaching its end, no
commands at all, or the special stage no longer being entered. `--record` is what
made that possible — a session played here is input in our own frame numbering,
so it replays for ever, where a foreign `.bk2` needs an alignment that provably
cannot be made to hold.

*The two 68000 backends against each other is delivered too*, as `--xcheck`, and
the design written here for it was wrong in a way worth keeping visible:

> ~~Compare commands posted, the frame buffer, VRAM, CRAM and the palette at
> checkpoints across a long headless run.~~

That would have been noise. The two backends do not stay in step and are not
meant to: the recompiler charges a block's cycles in its prologue where Musashi
charges each instruction as it retires, so a block straddling one of the sixteen
hand-overs in a scanline overshoots and the SH-2s get a slightly different slice.
Measured, they leave byte-identical VDP and 32X state at 300 frames and then
wander — 1,388 bytes of the 32X dump differ at frame 500 of a session, 128 at
1,000, 5,561 at 2,000. Wandering rather than growing is what says it is phase and
not a fault.

So it is lock-step instead: every recompiled block is re-run on Musashi from the
same registers, and what is compared is the registers it leaves, the address it
goes to and every byte it touched. Timing cannot enter into it. The banked window
put back is caught at frame 314 of the session, on the transfer itself — the
claim this list made, now tested. Zero divergences over 117,045,848 blocks of the
recorded session and 509,601,542 of the tuffcracker TAS; the 2,400-frame headless
run in `check` needs nothing but the cartridge, so unlike the session gate it
cannot skip. [done.md](done.md) has the
four things that had to be understood first, of which the load-bearing one is
that the recompiled 68000 works in cartridge offsets end to end and an address
exists only where `m68k_run` is handed one.

It found one fault of its own: the immediate of `ori`/`andi`/`eori` to CCR was
being read from the wrong word, so twenty sites ran `| 0x3C` where the cartridge
says `| 0x01`. No gate could have seen it — the listing round-trip passes because
the operand *text* was right, and only the recompiler's reading of it was wrong.

One piece is still missing.

* **More sessions.** The one baseline session covers a level, a special stage
  played to its end and the Combi Catcher. The save select, the player select,
  the attraction select and the rest of the hub are covered by nothing, and only
  playing produces the coverage. Sessions are local to each machine, in `roms/`
  with the cartridges and the logs, so a missing one **skips loudly** rather than
  printing a tick — the failure this project has already been bitten by once,
  when `build/sh2_recomp.c` had no prerequisites and every gate passed while
  measuring the previous front end.

  **Two-player sessions are now recordable and none exists.** Until this session
  nothing on the host could drive the second port — the pad hardware in
  `src/gen68k.c` has always been written over three ports and `--record` has
  always written both, but the keyboard only ever reached port 1, so every
  session ever recorded here is one player. That is a real hole in a game whose
  two characters are tied together by a ring, and it is the whole of the
  two-player engine: the partner's physics, the tether, and whatever the
  character-select does differently. See [state.md](state.md) for the keys.

*Deliberately not gated:* commands by kind, frame-buffer bytes and the three
instruction rates are printed against a baseline with their delta and never
enforced, because a scheduling or timing fix moves them legitimately and a gate
that broke on those would cost more than it catches. Replay is deterministic, so
today the drift is zero — which is what makes a nonzero one worth looking at.

### 2. No frontier is known

200,000 frames run clean and nothing has been run longer. The 31-minute
tuffcracker TAS plays all 111,397 of its frames with nothing to report — 1.3
billion 68000 instructions through five zones, no missing block, no stall — but
it desyncs inside the first level and never enters a special stage, and the
reason is upstream of any alignment: the movie's menu path is not ours, so what
the game enters the level holding need not be what was recorded. [done.md](done.md)
has the measurements.

It is still worth running under `--xcheck`, and that is not a contradiction: the
desync makes it useless as a statement about *playing* the game and changes
nothing about it as a supply of 68000 blocks to check translations against. Half
a billion of them agree, through five zones.

**And the reason it desyncs is now measured rather than suspected.** The
hypothesis above — that what the game enters the level holding need not be what
was recorded — has a mechanism, and it is the one-player/two-player half of it.

*The TAS is a two-player recording.* Its entire port-2 content is a single Start
at movie frame 366, between the port-1 presses at 361 and 459, which is how a
second player joins this game. In our run that press **does nothing at all**:
zeroing it produces a byte-identical run — same commands, same frame buffer, same
VRAM. Sweeping a synthetic port-2 Start across the whole menu phase, our run
accepts one in two narrow windows, movie frames **320-324** and **452-456**, and
366 is in neither. The screen sequence says the same thing from the other side:
our bitmap mode is `0001` over movie frames 340-430 and again 450-470, which is
not where the real machine's menu screens were.

So the game enters the level in one-player mode where the movie recorded two, and
from level frame 1 there is a second character being simulated in one run and not
the other. Moving the press to 456 changes the outcome completely — the run ends
up in Training rather than Isolated Island — which is the confirmation that the
press is load-bearing, not the fix.

*What the fix would have to be, and why no flag here is it.* Neither
`--movie-offset` nor `--movie-resync` can help, because both move the movie's
menu presses and its level start together, and the problem is that our menu is a
different menu at a different time. It would have to be **our own** presses
driving the menu into two-player Scenario Quest with the right character pair,
and the movie handed control only at its first gameplay input at frame 633 —
which the existing flags can already express (an offset far enough out that no
movie input plays early, `--press`/`--hold2` to navigate, then
`--movie-resync <our level frame>:633`). What is not known is the press sequence
that gets our menu there, and the character pair and the movie's two pause
presses are still upstream of it. It is an afternoon of screenshots, and the
prize is one foreign recording that still could not be a gate.

### 3. The SH-2s keep time with one number each

`sh2_cpi1000` is 1.634 and 1.009 cycles an instruction, measured over whole
reference frames. The 68000 has already been through this: `recomp_cpi` was 11,
exactly right in the steady state and 18% fast through the boot, and the fix was
for each block to carry what its own instructions cost, summed from Musashi's own
table at build time. The SH-2 side is the same shape and harder — there is no
Musashi to take a table from, and an SH-2's cost is mostly its cache.

It is what the largest remaining trace group is made of. The command rendezvous
prices the master's acknowledgement at 29 of its cycles where the reference takes
about 135, because that path is nearly all memory — PC-relative loads out of
SDRAM, writes into the 32X register block — and a frame average dominated by
cache-resident tight loops charges a quarter of what it really costs.

### 4. The banked window is wholly interpreted

`canon68k` refuses `0x900000`-`0x9FFFFF`, because which megabyte of the cartridge
it shows is a register, so every call the engine makes into bank 2 goes to
Musashi — 988,493 hand-overs in 3,600 frames. It is correct and it is not free;
the run is still faster than the interpreted build, 5.6 seconds against 8.0 for a
minute of game, so this is a cost to measure rather than an emergency. The real
fix is to key translation by *(bank, offset)* rather than offset, which means
four images of the window in the front end and a dispatch that reads the
register — a front-end change, not a runtime one.

### 5. The sound has no accuracy gate, only an input gate

Every byte that reaches the two Mega Drive chips is the reference's and the PWM
is held sample for sample, but what comes *out* of the chips is unmeasured
against anything: the reference logs instructions, not audio. Nuked-OPN2 removes
the question for the FM and the PSG has an arithmetic test of its own. The mix is
what is left, and it is chosen rather than measured — the PWM has the output's
full swing, four PSG channels at full volume half of it, and the FM is scaled by
six, measuring 2,562, 816 and 1,642 RMS at 1,800 frames. What the three are worth
against each other on real hardware is an analogue question that would stay a
judgement even with a better oracle.

### 6. The slave diff is bounded by trace size

Still 2,000 reference blocks, and what limits it is how far our own trace
reaches, not the aligner. It is roughly linear — 2,000,000 lines cross about
2,500 blocks and 8,000,000 about 7,000 — so raising `--trace-sh2-lines` and the
bound together is the lift, and it costs disk rather than cleverness. 20,000
would want some 21 million lines and a twelve-gigabyte file.

## Open, found by playing rather than by any gate

* **The ring pickup sound is missing.** All four sources are live in a level —
  the PWM at 366 interrupts a frame with the sample changing, the PSG audible,
  33,000 YM2612 register writes, the Z80 running its driver — so this is one
  effect and not a dead path. Nothing here can gate it: the reference logs are
  the boot and are constant silence, so "never fires" and "fires inaudibly" are
  not distinguishable without a person listening. It is not the comm-register
  byte write either; that fix recovered 19 sounds in a level, where a ring is
  picked up far more often than that.
* **A 32X vertical interrupt that arrives while its own handler is running is
  dropped**, where hardware would hold it pending. Only the special stage enables
  that interrupt at all, which is why nothing has surfaced from it yet.

## Carried debts

*The ones still open. The settled ones, and what each turned out to be, are in
[done.md](done.md). Items 4 and 5 above are debts too and are not repeated
here.*

* **The vertical interrupt's position inside line 224 is not measured.** We
  raise it at the top of the line and the 68000 takes it at the first
  instruction boundary, which is what the reference's own markers look like:
  102 of them, all `@ 224,n` with n between 3 and 8 of the ~211 hpos units in a
  line, or 7 to 18 cycles in. Never 0 to 2, which hints the raise itself is a
  unit or two past the line's start rather than on it — but that is a couple of
  cycles inside a 22-cycle wait loop, and it cannot be told apart from where the
  reference emulator happened to have an instruction boundary. 38 rows at
  `0x8834C0`/`0x8834C4`, down from 58, are this loop's phase.
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
* **The recompiled 68000's block gate lost half its agreement to the divide
  unit.** 8,741 blocks to 4,847, with divergences 384 to 493, while the
  instruction-granular interpreted gate went the other way, 389 divergences to
  361. The boot halves are identical either way — 2,466 blocks, 23 divergences
  — so all of it is past the boot, where the master's timing decides which
  branch the vblank handler takes, and where one register difference costs an
  instruction of agreement in the fine gate against a whole block in the coarse
  one. Reporting the coarse gate's agreement in instructions rather than blocks
  would make the two comparable and is probably the honest fix.
* **The recompiled 68000's PC is not masked to 24 bits.** At the frontier it
  holds `0xFFFFDD32`, which `canon68k` passes through unchanged; it works only
  because the lookup then fails and the interpreter masks it on the way in.
  Accidental rather than designed. Its data-side twin is that address arithmetic
  reaches `src/gen68k.c` unmasked as well — a `movem.l (a6)` with a6 holding a
  sign-extended `0xFFFFFFC0` arrives as exactly that, and the mask at the door is
  what makes it work. `--xcheck` records in bus addresses for this reason.
* **The recompiled 68000 works in cartridge offsets, including on the game's own
  stack.** A `bsr` pushes an offset where Musashi pushes an address, and after a
  fuel yield `m68k_interrupt` pushes one too. It is self-consistent — an `rts`
  dispatches it back through `canon68k` — and it works across a hand-over only
  because the cartridge is aliased at `0x000000` and `0x880000`, so the
  interpreter resuming at an offset reads the same bytes. The same family as the
  unmasked PC above: it has never been wrong, and nothing makes it right. What
  would settle it is the offset-keyed banked window in §4, which has to decide
  what a translated address *is* anyway.
* **`0x0749C2`-`0x0749D0` is 68000 code discovery has never found.** Not a
  block, not in `az.code`, between known blocks at `0x749AA` and `0x749E4`, and
  entered 4,163 times in 3,600 frames. `coverage` is clean because it is only
  ever asked of traces that end at 1.7 seconds.
* **Genesis VDP gaps:** no window plane, shadow/highlight, interlace, or the
  per-line sprite and pixel limits.

## Not worth doing, and measured rather than assumed

The point of this list is that each line cost an experiment, so the next session
does not spend the same day finding the same thing.

* **A wait-state penalty for crossing the adapter.** There is none to charge.
  Six loops on the PWM clock, three reading work RAM and three reading a 32X
  register, all within 0.4% of what the 68000 manual charges — [done.md](done.md)
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
* **Holding a button to test a menu.** `--hold` presses from frame zero and
  therefore never *becomes* pressed, which is the edge a menu acts on. Every
  hold tried against the title screen changed nothing at all; `--press`, which
  pulses, skipped the cutscene on the first run.
* **The long master extract as a discriminator.** `diffsh2.py --cpu master --ref
  build/ref-long-shm.txt --ref-blocks 20000` returns 215 blocks agreed and 3,432
  divergences, and returns *exactly that* with the divide unit in and with it
  out. It walks the whole extract and cannot tell the two apart, so it is not
  the instrument for anything past the boot as configured.
* **Comparing the two 68000 backends by running the game twice.** They leave
  byte-identical state at 300 frames and then wander, because the recompiler
  charges a block's cycles in its prologue and Musashi charges each instruction
  as it retires. It is phase, not fault, and no checkpoint scheme gets around
  it — see the measurements in §1. Lock-step per block is what works.
* **Deriving the address a recompiled block was reached through.** The window it
  came in by can be carried from the dispatch, but it collapses on the first
  `bsr`: the generated code returns offsets and pushes offsets, so an address
  exists only where `m68k_run` is handed one. A shadow run at a reconstructed
  address reports every subroutine call in the game. The fold gets asked about
  separately, at the dispatch, where both numbers are real.
* **A VDP bus-contention model** — the first attempt at the 68000's clock. It
  changed the instruction count by nothing at all, which is what said the budget
  was not what governed. See the note above `cpu_credit` in `src/mars_main.c`.

## Verification strategy

Ground truth is what makes or breaks a project like this, and the original plan —
run the same cartridge in a known-good emulator, log every CPU state transition
and every hardware access, replay the same trace through the recompiled build,
and let divergence name the instruction that broke — is built. It is what found
most of what [done.md](done.md) records, and it is why building it early was
cheaper than debugging a black screen later. It comes to the gates in
`make check`: two byte-exact listing round-trips on both cartridges, three
semantics and arithmetic suites, five trace diffs against the reference, a
sample-for-sample PWM comparison, the bytes reaching the two Mega Drive chips,
front-end coverage, the movie round trips, and a replayed play session.

Its one structural limit is the whole of what M7 owes: **the reference logs are
1.7 seconds of a game that runs for two hundred and fifty**, and every fault past
the SEGA logo has been outside them. So the second half of the strategy is
oracles that need no reference at all — the runtime's own invariants, a replayed
session that carries the game where no headless run reaches, and the two 68000
backends checked against each other. All three now exist, and the third has
already found something none of the others could see.

What the third does *not* cover is worth being exact about, because it is easy to
read `--xcheck` as more than it is. It holds the recompiled 68000 to Musashi and
nothing else: an SH-2 translation, the Z80, either VDP, the scheduling between
them and every sound path are outside it entirely, and so is anything both
backends get wrong the same way. It is one CPU checked against one other reading
of the same instruction set.

## Honest scope note

M0–M6 are done and the game plays. What is left is accuracy, coverage and speed
rather than a missing part of the machine: nothing above is blocked on an
unknown, the architecture is documented, and the code is statically reachable
except where the engine writes it at run time — which is why an interpreter sits
beside the recompiler and always will.

The risk that remains is not on the list. It is that past 1.7 seconds the only
instrument this project has ever had is a person playing the game. Gates first.
