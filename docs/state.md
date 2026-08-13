# State

Facts only. Reasoning, history and rejected approaches are in
[roadmap.md](roadmap.md).

Generated from the build at commit `edc5301` plus this session's fixes — the
dropped delay slot, the 32X priority bit, the 32X vertical interrupt and the
script-dispatch front-end rule. `make check` passes.

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
| Frame loop, scheduling, clocks | `src/mars_main.c` |

`--interp` swaps the whole 68000 to Musashi.

## Front ends

| | SH-2 | 68000 |
|---|---|---|
| functions | 801 | 4,491 |
| basic blocks | 2,778 | 7,944 |
| instructions | 11,816 | 24,579 |
| dispatch tables | 15 | — |
| unresolved indirect transfers | 22 | 45 |
| coverage | 71.3% of the SDRAM image | 2.8% of the 3 MB ROM |

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
| 68000 whole extract (`diff68k.py`) | 52,525 of 54,081, 344 divergences |
| master SH-2 (`diffsh2.py --cpu master`) | 197 of 200 blocks, 18 divergences |
| slave SH-2 (`diffsh2.py --cpu slave`) | 1,344 of 2,000 blocks, 593 divergences |
| PWM output (`diffpwm.py`) | 3,361 of 3,361 samples exact |
| Z80 (`diffz80.py`) | 32,622 of 33,984, 330 divergences |
| chip register input (`diffz80.py`) | 164 of 164 bytes, in order |
| recompiled 68000, blocks (`diff68k.py --blocks`) | 3,376 of 9,848 blocks, 599 divergences |
| recompiled 68000, boot only | 2,466 blocks, 23 divergences |
| front-end coverage (`disasm68k.py coverage`) | clean |

No fatal divergence in any diff. Every diff walks its whole extract.

**All of them stop at 1.7 seconds of game** — that is the length of the
reference logs. Nothing gates anything after that.

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

Commands a frame varies with the scene: about 1.0 in a level and 0.5 on the
title screen, against the reference's 0.83 over its own 1.7 seconds. Zero
unmapped accesses on either side across the whole run.

Sound: all three sources play. At 1,800 frames the PWM is 2,562 RMS, the PSG
816, the YM2612 1,642, all of it 3,162, nothing clipped.

Speed: 1,800 frames headless in 2.4 s, about 12x real time. With a window the
audio device paces it to the machine's speed.

## Command line

```
./build/mars [rom] [flags]
```

| Flag | Effect |
|---|---|
| `--frames N` | headless, N frames, writes `build/frame.ppm` |
| `--interp` / `--recomp` | Musashi or recompiled 68000 (recompiled is the default) |
| `--hold LIST` | hold buttons from frame 0 (`up,down,left,right,a,b,c,start,x,y,z,mode`) |
| `--press LIST` | pulse buttons, 15 frames in every 45 — what a menu needs |
| `--press-until N` | stop pulsing at frame N, so a run can sit on the screen it reached |
| `--layers N` | 1 plane B, 2 plane A, 4 sprites, 8 the 32X bitmap |
| `--sound N` | 1 PWM, 2 PSG, 4 YM2612 |
| `--audio` / `--mute` | force a device on / off |
| `--wav FILE` | write the machine's own sample stream |
| `--rate68k` | 68000 instructions a frame, per frame |
| `--trace68k FILE`, `--trace-sh2 FILE`, `--trace-z80 FILE`, `--trace-pwm FILE`, `--trace-chips FILE` | traces for the diff tools |
| `--trace-from N` | start every tracer at frame N |
| `--progress N` | one liveness line every N frames — commands, PCs, bitmap mode, interrupt masks, unmapped accesses |
| `--hold-from N` | start holding at frame N, so the menus are passed untouched |
| `--watch ADDR[:LEN]` | log SH-2 writes into that range, with the block they came from |
| `--trace68k-lines N`, `--trace-sh2-lines N`, `--trace-z80-lines N` | line budgets |
| `--dump-vdp FILE`, `--dump-32x FILE`, `--dump-z80 FILE`, `--dump-sdram FILE` | memory snapshots |
| `--trace` | SH-2 function-entry counts and the last block ring |

Keyboard, windowed: arrows, `Z`/`X`/`C` = A/B/C, `A`/`S`/`D` = X/Y/Z, `Enter`
= Start, `Tab` = Mode, `Esc` quits.

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

Reference logs live in `roms/*.log` (6.5 GB, not in the repository); the
extracts the gates read are rebuilt into `build/` on demand.

## Open

Ordered as in the roadmap's plan.

1. No gate exists past 1.7 seconds. `--progress` watches liveness, and a stall
   now reports itself without a flag, but nothing fails a build for either, and
   the third oracle — the two 68000 backends against each other — is not
   written. All three bugs this session cost a play session each to find.
2. No frontier is known. 200,000 frames run clean; nothing has been run longer.
3. `sh2_cpi1000` is one cycles-per-instruction number per SH-2.
4. The banked window is wholly interpreted — 988,493 hand-overs in 3,600 frames.
5. The sound mix is chosen, not measured; the chips have no output gate.
6. The slave diff is bounded at 2,000 reference blocks by trace size.

Also open: a 32X vertical interrupt that arrives while its own handler is
running is dropped where hardware would hold it pending; the 32X's priority is
the bitmap mode register's bit 7 only, with no per-pixel priority modelled; `0x0749C2`-`0x0749D0` is 68000 code discovery has
never found; the recompiled 68000's PC is not masked to 24 bits; the Genesis
VDP has no window plane, shadow/highlight, interlace or per-line sprite limits;
Genesis DMA fill and copy are skipped.
