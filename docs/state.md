# State

Facts only. What is left to do, and what was tried and rejected, is in
[roadmap.md](roadmap.md); how each of it was arrived at is in [done.md](done.md).

Generated from the build at commit `ab57699` plus `--xcheck` and the one thing
it found. `make check` passes.

## What this is

Static recompilation of *Knuckles' Chaotix* (32X) to native ARM64, with a
purpose-built runtime for the hardware. Not an emulator port: the SH-2 and 68000
code is translated to C at build time and compiled.

## What runs what

| Part | Implementation |
|---|---|
| 68000 | recompiled C (`build/m68k_recomp.c`), default |
| 68000, three regions | Musashi interpreter — below `0x000100`, work RAM `0xFF0000`, banked window `0x900000`-`0x9FFFFF` |
| SH-2 master + slave | recompiled C (`build/sh2_recomp.c`) |
| Z80 | interpreter (`src/z80.c`) |
| Genesis VDP | `src/gen68k.c` state, `src/genvdp.c` raster |
| 32X VDP, SDRAM, DREQ, PWM | `src/mem32x.c` |
| YM2612 | Nuked-OPN2 (`third_party/`, not in the repository) |
| PSG | `src/psg.c` |
| Cartridge SRAM | `src/gen68k.c`, saved to `<cartridge>.sav` |
| Frame loop, scheduling, clocks | `src/mars_main.c` |

`--interp` swaps the whole 68000 to Musashi.

## Front ends

| | SH-2 | 68000 |
|---|---|---|
| functions | 1,884 | 4,491 |
| basic blocks | 3,718 | 7,944 |
| instructions | 11,992 | 24,579 |
| dispatch tables | 15 | — |
| unresolved indirect transfers | 42 | 45 |
| coverage | 72.3% of the SDRAM image | 2.8% of the 3 MB ROM |

Both cartridges (JU and E) reassemble byte for byte from the emitted listing.

## Gates — `make check`

| Gate | Result |
|---|---|
| SH-2 listing round-trip, JU and E | 36,864 + 1,024 bytes identical |
| 68000 listing round-trip, JU and E | 3,145,728 bytes identical |
| SH-2 semantics (`test_recomp.py`) | 8 cases pass |
| 68000 semantics (`test_recomp68k.py`) | 38 cases pass |
| PSG arithmetic (`test_psg.py`) | 18 cases pass |
| 68000 boot (`diff68k.py --ref-lines 20213`) | 20,084 of 20,178, 19 divergences |
| 68000 whole extract (`diff68k.py`) | 52,524 of 54,081, 346 divergences |
| master SH-2 (`diffsh2.py --cpu master`) | 172 of 200 blocks, 31 divergences |
| slave SH-2 (`diffsh2.py --cpu slave`) | 1,204 of 2,000 blocks, 409 divergences |
| PWM output (`diffpwm.py`) | 3,361 of 3,361 samples exact |
| Z80 (`diffz80.py`) | 32,622 of 33,984, 330 divergences |
| chip register input (`diffz80.py`) | 164 of 164 bytes, in order |
| recompiled 68000, blocks (`diff68k.py --blocks`) | 3,376 of 9,848 blocks, 599 divergences |
| recompiled 68000, boot only | 2,466 blocks, 23 divergences |
| the two 68000 backends (`--xcheck`) | 12,811,820 blocks re-run on Musashi, 13,271 windowed transfers, 0 divergences |
| front-end coverage (`disasm68k.py coverage`) | clean |
| played session (`test_session.py`) | 27,741 frames, special stage entered, no missing block — local input, skips without one |

No fatal divergence in any diff. Every diff walks its whole extract, and
`diffsh2.py` still ends on "every instruction the reference ran is inside a
block we have".

**Two of those gates do not stop at 1.7 seconds.** `--xcheck` re-runs every
recompiled block on Musashi from the same registers and compares the registers
it leaves, the address it goes to and every byte it touched; `test_session.py`
replays a recorded play session. The 2,400 frames in `check` are the headless
version, which needs nothing but the cartridge and so cannot skip. It costs about
three times the run time, so the long runs are worth doing by hand:

| `--xcheck` over | blocks re-run | windowed transfers | divergences |
|---|---|---|---|
| `check`, 2,400 frames headless | 12,811,820 | 13,271 | 0 |
| the recorded session, 27,741 frames | 117,045,848 | 913,455 | 0 |
| the tuffcracker TAS, 111,397 frames at offset 500 | 509,601,542 | 3,153,331 | 0 |

The TAS is worth its eight minutes even though it desyncs and never reaches a
special stage: it is half a billion blocks through five zones, and what it is
being asked is whether a translation is right, which does not care that the
character is being driven badly.

The two SH-2 block gates read lower than they did at 251 SH-2 functions — 197
and 1,344 — and that is the gate seeing more rather than the runtime doing
worse. The reference stream is filtered to *our* block addresses, so finer
blocks make it finer too, and differences that had no row before now have one:
three of the master's are "control flow parts; N reference blocks we never run",
at addresses that were not blocks at all. It is an interpretation, not a
proof.

**All the reference diffs stop at 1.7 seconds of game** — that is the length of
the reference logs. `test_session.py` is the one gate that does not need them: it
replays `roms/session.movie`, a `--record`ed play session, which for the baseline
below was 27,741 frames covering a level, a special stage played to its end, and
the Combi Catcher.

That session is **local, not in the repository** — it lives in `roms/` with the
cartridges and the reference logs, for the same reason. Make your own by playing
with `./build/mars --record roms/session.movie`; with none present the gate
**skips and says it measured nothing**, rather than printing a tick. A
differently-recorded session still gets the loose conditions; `--rebaseline`
prints a replacement baseline block for its numbers.

It is **loose on purpose**. What fails a build is only what is always a bug — a
transfer with no recompiled block, a CPU parked at zero, an unmapped SH-2
access, the movie not reaching its end, no commands at all, or the special stage
no longer being entered. Commands by kind, frame-buffer bytes and instruction
rates are printed against their baseline with the delta and never enforced,
because a scheduling or timing fix moves them legitimately. Replay is
deterministic, so today every number matches exactly; a drift is a prompt to
look, not a break. A recovered stall report — the results screen holds for 240
frames at frame 23,004 — is printed too; a terminal one fails as a parked CPU or
as no commands.

## Clocks

| | Value | Source |
|---|---|---|
| frame | 128,006 68000 cycles | 896,040/7, NTSC 59.9227 Hz |
| scanline | 262 a frame | NTSC |
| hand-over | 16 a scanline | measured, 32 and 64 are worse |
| SH-2 | 3x the 68000 | hardware |
| SH-2 cycles/instruction | 1.634 master, 1.009 slave | reference frame counts |
| 68000 cycles/instruction | per block, from Musashi's table | `tools/m68kcycles.c` |
| Z80 | 7 cycles per 15 of the 68000 | master clock / 15 |
| PWM period | `(cycle - 1) * TM` SH-2 cycles | 1,046 here, 22.0 kHz |
| YM2612 | 68000 clock / 144 | 888.93 samples a frame |
| vertical interrupt | line 224, held until acknowledged | reference markers |
| 32X autofill | 2 SH-2 cycles a word | reference clear loop |

## Measured rates, against the reference

| | ours | reference |
|---|---|---|
| 68000 instructions a frame | 11,282 | 11,611 steady |
| master SH-2 a frame | 234,845 | 235,044 |
| slave SH-2 a frame | 380,314 | 380,724 |
| Z80 between vertical interrupts | 9,549 | 9,528 |

## How far the game gets

| Frame | State |
|---|---|
| ~300 | SEGA logo, correct — 15,116 frame-buffer bytes, 224/224 line table, 362 palette bytes |
| ~1,800 | title screen, complete: logo, `PUSH START`, five characters, copyright |
| ~2,400 | attract mode, first level, with HUD, both characters and the ring tether |
| 200,000 | still going — the attract loop, level after level, no stall |

`--press start --press-until 350` goes through the title and the save select
into a playable level instead of watching the attract loop.

Played from the keyboard it goes further than any headless run reaches: the
save select and the player select render, a level plays and can be completed,
the special stage draws its tunnel, its HUD and the molecule background and is
playable to its Chaos Ring, and the Newtrogic High Zone hub, the Combi Catcher
and the attraction select all draw. What is known to be wrong there is in Open
below.

Commands a frame varies with the scene: about 1.0 in a level and 0.5 on the
title screen, against the reference's 0.83 over its own 1.7 seconds. Zero
unmapped accesses on either side across the whole run.

Sound: all three sources play. At 1,800 frames the PWM is 2,562 RMS, the PSG
816, the YM2612 1,642, all of it 3,162, nothing clipped. Effects reach the Z80's
SMPS queue at 0xA01C0A, including the ones the master posts through comm 0 —
60 of them in the special stage, which used to get none.

Save data: the cartridge's 512 battery-backed bytes at 0x200001-0x2003FF, odd
addresses, mapped over the ROM by 0xA130F1. Written to `<cartridge>.sav` when
the game commits, and the file the game writes from an empty battery is byte for
byte the one the reference emulator wrote.

Speed: 1,800 frames headless in 2.4 s, about 12x real time. With a window the
audio device paces it to the machine's speed.

## Recorded input

`tools/bk2.py` turns a BizHawk `.bk2` into one line of input per frame and
`--movie` plays it back; `make tas` does both for whatever `.bk2` is in `roms/`.

The one to hand is `tuffcrackerv3-knuckleschaotix-tuffcracker.bk2`: 111,397
frames, 31 minutes, recorded on PicoDrive against the JU cartridge. It plays to
the end in 3.7 minutes. **`--movie-offset 500` is the value to use**; 0 does not
start a game at all.

```
./build/mars --movie build/tuffcrackerv3-knuckleschaotix-tuffcracker.movie \
    --movie-offset 500 --frames movie --progress 2000 --shots 1000
```

At offset 500, over all 111,897 frames:

| | |
|---|---|
| frames played | 111,397 of 111,397 |
| commands | 89,478 posted, 89,479 serviced |
| missing blocks | none |
| stall reports | none |
| unmapped | sh2 0, 68k 1 (the mapper register at `0xA130F0`) |
| rates a frame | master 235,011 / slave 380,583 / 68000 11,748 — the reference's to within 0.15% |
| how far it plays | the save select, then Isolated Island for 10 minutes of level clock |
| special stages | none — all 55 `--progress` lines read `v 0/0`, and only the special stage enables the 32X vertical interrupt |

**Why the offset is load-bearing.** The movie was recorded against the real 32X
firmware, which this runtime stands in for rather than runs, so movie frame 0 is
not our frame 0. The movie's whole menu sequence is four single-frame Start
presses, at movie frames 162, 328, 361 and 459, and at offset 0 they land on the
SEGA logo and the intro instead of the title. The save select is then never
reached — bitmap mode is `0081` on all 55 progress lines of a 31-minute run, and
`0001` is the save select — so nothing the movie does after frame 633 is playing
the game, and the levels such a run shows are **attract-mode demos**.

Offsets 400-700 all get through, and 500 is the middle of that plateau. Below 400
it is patchy: 225, 300 and 375 fail where their neighbours pass, a single-frame
press falling in a window a few frames wide.

**It still desyncs inside the level.** At 500 the menu is entered once and never
returned to, the level clock runs 0'13" to 9'50" over frames 2,000 to 40,000, and
the score stays 0 throughout — the character is being driven and is not
progressing — and Isolated Island ends on TIME OVER at 9'59".

Measured, in movie coordinates: our menu screen ends at movie frame **420 or 520**
depending on the offset and at nothing in between, and our level's first frame is
movie frame **665** where the movie's first gameplay input is at **633**. A global
offset cannot close those 32 frames, because it moves the movie's presses and the
level start together and the phases it can reach are 100 frames apart.

`--movie-resync 1160:633` is what aligns it, and phase alone is not enough:
sweeping the movie frame from 601 to 673 changes the run substantially — 82-86% of
pixels differ at frame 6,000 — and the score stays 0 in every one. Two things sit
upstream of the phase and would have to be settled first:

* **The movie pauses the game.** Its Start presses at movie frames 2329 and 5726
  are a pause and an unpause, so at offset 500 our frames 2,829-6,226 are paused.
  Any probe window has to avoid them.
* **The menu path is not the movie's.** The `0001` screen is SCENARIO QUEST, and
  our run leaves it at movie frame 420 or 520 while the movie's own confirming
  press is at 459 — so different presses are ending it, and what the game enters
  the level with need not be what was recorded. If the character pair or the
  one/two-player mode differs, the physics differ from level frame 1 and no
  alignment can sync.

`--record FILE` writes the same text back out: both pads, one line per frame,
whatever composed them — keyboard, `--hold`/`--press`, or a replay. It is the
way out of the offset problem entirely, because a session played in our own
window is input in our frame numbering: it replays at offset 0 with every
press on the frame it was made on, and the log is committable text. Two
`check` gates hold the round trip — a generated movie carrying every 12-bit
mask on both ports records back identical through a replay, and a recorded
`--press`/`--hold` run replayed with the recorder still on copies its file
byte for byte. Recording a replay bakes the alignment in: the tuffcracker
movie played at `--movie-offset 500` and recorded comes back as a file that
replays plain. What no offset or re-sync of a foreign movie ever reached — the
special stage — one played session reached in a single recording, and that
recording is what `test_session.py` replays.

`--shots N` writes `build/shots/fNNNNNN.ppm` through the run, which is how a
31-minute run gets looked at. `ffmpeg -pattern_type glob -i 'build/shots/*.ppm'
build/tas.mp4` makes it a video.

`--show-input` puts the pad on the picture, bottom left: the twelve buttons lit as
they are held, a second row when the movie has two players, and the two frame
counters. It is the direct answer to "is this input reaching the game" — at
`F1191 M691` in Isolated Island the display shows `R` and `C` lit, which is what
movie frame 691 holds. `make tas` turns it on, so every shot carries it.

## Command line

```
./build/mars [rom] [flags]
```

| Flag | Effect |
|---|---|
| `--frames N` | headless, N frames, writes `build/frame.ppm` |
| `--frames movie` | headless for exactly as long as `--movie` is |
| `--interp` / `--recomp` | Musashi or recompiled 68000 (recompiled is the default) |
| `--hold LIST` | hold buttons from frame 0 (`up,down,left,right,a,b,c,start,x,y,z,mode`) |
| `--press LIST` | pulse buttons, 15 frames in every 45 — what a menu needs |
| `--hold2 LIST`, `--press2 LIST` | the same for the second pad, which is also the only way to exercise port 2 without a person at a keyboard |
| `--press-until N` | stop pulsing at frame N, so a run can sit on the screen it reached; bounds both pads |
| `--movie FILE` | play a recorded run, one line of input per frame, from `tools/bk2.py` |
| `--movie-offset N` | which of our frames plays the movie's frame 0; negative skips into it |
| `--movie-resync OURS:MOVIE` | from our frame OURS, play movie frame MOVIE — re-align part way in; repeatable, last match wins |
| `--record FILE` | write both pads back out, one line per frame, in the text `--movie` reads — a played session becomes a movie in our own frame numbering, and recording a replay bakes its offset and re-syncs into the file |
| `--save FILE` | where the cartridge's battery lives; the default is `<cartridge>.sav`, and a `--movie` replay uses no battery at all unless this names one |
| `--no-save` | run on a battery that outlives nothing |
| `--shots N` | a rendered frame every N frames, into `build/shots/` |
| `--show-input` | draw the pad on the picture — the twelve letters lit as held, our frame `F` and the movie frame `M` it played, so `F` minus the offset is `M`. In the window, in `--shots` and in `build/frame.ppm` |
| `--layers N` | 1 plane B, 2 plane A, 4 sprites, 8 the 32X bitmap |
| `--sound N` | 1 PWM, 2 PSG, 4 YM2612 |
| `--audio` / `--mute` | force a device on / off |
| `--wav FILE` | write the machine's own sample stream |
| `--rate68k` | 68000 instructions a frame, per frame |
| `--trace68k FILE`, `--trace-sh2 FILE`, `--trace-z80 FILE`, `--trace-pwm FILE`, `--trace-chips FILE` | traces for the diff tools |
| `--trace-from N` | start every tracer at frame N |
| `--progress N` | one liveness line every N frames — commands, PCs, bitmap mode, interrupt masks, unmapped accesses |
| `--hold-from N` | start holding at frame N, so the menus are passed untouched |
| `--xcheck` | re-run every recompiled block on Musashi from the same registers and compare — the oracle that needs no reference. Exits non-zero on a divergence, and costs about 3x |
| `--xcheck-stop` | the same, stopping at the first one |
| `--survive-missing` | carry on at PR when a transfer has no block, instead of parking — the run is wrong from that point, but one session surfaces several addresses instead of one |
| `--watch ADDR[:LEN]` | log SH-2 writes into that range, with the block they came from |
| `--trace68k-lines N`, `--trace-sh2-lines N`, `--trace-z80-lines N` | line budgets |
| `--dump-vdp FILE`, `--dump-32x FILE`, `--dump-z80 FILE`, `--dump-sdram FILE` | memory snapshots |
| `--trace` | SH-2 function-entry counts and the last block ring |

Keyboard, windowed — two players, because this game's two characters are tied
together by a ring:

| | player 1 | player 2 |
|---|---|---|
| d-pad | arrows | `I`/`K`/`J`/`L` |
| A B C | `Z`/`X`/`C` | `V`/`B`/`N` |
| X Y Z | `A`/`S`/`D` | `F`/`G`/`H` |
| Start | `Enter` | `RShift` |
| Mode | `Tab` | `RCtrl` |

`Esc` quits. Game controllers are opened through SDL's controller layer — which
is what covers XInput and DirectInput on Windows, IOKit on macOS and evdev on
Linux, with nothing per-platform here — and fill the ports in the order they
arrive, hot-plug included. The left stick works as a d-pad past half deflection.
The keyboard stays live on both ports whatever is plugged in, so a pad and a
keyboard is two players.

On a modern pad: `X`/`A`/`B` are A/B/C, `Y` and the two shoulders are X/Y/Z,
Start is Start and Back is Mode.

The summary line `pads polled: port 1 N, port 2 M` says how often the game asked
each port for its lines, which is the only way to tell "the second pad does
nothing" apart from "the game is not in two-player mode". In a level both read
17,600 in 2,400 frames, and across the recorded session both read 212,072: the
game polls the second port exactly as often as the first, always.

## Tools

| | |
|---|---|
| `tools/disasm.py` | SH-2 discovery and disassembly |
| `tools/disasm68k.py` | 68000 discovery, listing, `coverage` |
| `tools/emit_asm.py --verify` | SH-2 listing round-trip |
| `tools/recompile.py`, `tools/recompile68k.py` | the two recompilers |
| `tools/diff68k.py`, `tools/diffsh2.py`, `tools/diffz80.py`, `tools/diffpwm.py` | trace comparison against the reference |
| `tools/tracediff.py` | the alignment the diffs share |
| `tools/refrate.py`, `tools/refpoll.py`, `tools/refframe.py` | measurements taken from the reference logs |
| `tools/test_recomp.py`, `tools/test_recomp68k.py`, `tools/test_psg.py` | semantics and arithmetic tests |
| `tools/validate_decoder.py`, `tools/validate_m68k.py` | decoders against objdump |
| `tools/bk2.py` | a BizHawk `.bk2` movie to one line of input per frame |

Reference logs live in `roms/*.log` (6.5 GB, not in the repository); the
extracts the gates read are rebuilt into `build/` on demand.

`make tas` plays whatever `.bk2` is in `roms/` — headless, with `--progress` and
`--shots`. It is not part of `check`; see Recorded input below.

## Open

Ordered as in the roadmap's plan.

1. Past 1.7 seconds there are two gates. `--xcheck` holds the recompiled 68000
   to Musashi block by block and needs no session at all, but it says nothing
   about the SH-2s, the Z80, either VDP or the sound. `test_session.py` reaches
   all of those and enforces liveness rather than end-of-run numbers, over one
   recorded session. Scenes outside that session — the save select, the player
   select, the attraction select, the rest of the hub — are covered by nothing
   but `--xcheck`'s 68000 half, and only playing produces the rest.
2. No frontier is known. 200,000 frames run clean; nothing has been run longer.
3. `sh2_cpi1000` is one cycles-per-instruction number per SH-2.
4. The banked window is wholly interpreted — 988,493 hand-overs in 3,600 frames.
5. The sound mix is chosen, not measured; the chips have no output gate.
6. The slave diff is bounded at 2,000 reference blocks by trace size.

Open from play, not from any gate:

* **The ring pickup sound is missing.** All four sources are live in a level —
  PWM at 366 interrupts a frame with the sample changing, PSG audible, 33,000
  YM2612 register writes, the Z80 running its driver — so this is one effect,
  not a dead path. Nothing here can gate it: the reference logs are the boot and
  are constant silence, so "never fires" and "fires inaudibly" are not
  distinguishable without a person listening. Not the comm-register fix either:
  that recovered 19 sounds in a level, where a ring is picked up far more often
  than that.

Also open: a 32X vertical interrupt that arrives while its own handler is
running is dropped where hardware would hold it pending; the recorded session
makes one unmapped 68000 read, `0x8401FE`, in the 32X frame-buffer window;
`0x0749C2`-`0x0749D0` is 68000 code discovery has never found; the recompiled
68000's PC is not masked to 24 bits; the Genesis VDP has no window plane,
interlace or per-line sprite limits; Genesis DMA fill and copy are skipped.
