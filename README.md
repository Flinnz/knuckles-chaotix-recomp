# Knuckles Chaotix Recomp
> [!WARNING]
> I used many LLM dark spells making this project, please be patient.

> [!NOTE]
> If you don't mind usage of dark magic in repo, please feel free to contribute.

Reverse-engineering Knuckles' Chaotix (Sega 32X, 1995) toward a natively
recompiled ARM build.

Cartridge images are not in the repository — put your own dumps in `roms/`.

## Layout

```
tools/            analysis pipeline (Python, no dependencies)
  mars.py         32X cartridge model + per-CPU address maps
  sh2/decode.py   SH-2 instruction decoder
  sh2/analyze.py  recursive-descent code discovery
  disasm.py       CLI front end
  validate_decoder.py
toolchain/        locally built sh-elf binutils
docs/             state.md (current numbers), architecture findings, roadmap
```

## Setup

The two C dependencies are submodules, pinned at the commits this is built
against. A fresh clone wants `--recurse-submodules`; an existing one wants:

```bash
git submodule update --init
```

Musashi (MIT, Karl Stenerud) provides the 68000 interpreter; `src/m68kconf.h`
overrides its config down to a bare 68000 so the submodule stays untouched.

Nuked-OPN2 (LGPL 2.1, Alexey Khokholov) provides the YM2612. It is derived from
a die shot and is what every other implementation is measured against, which
matters here because a sound chip has no trace oracle: the reference logs
instructions, not audio. Everything else that makes a sound — the 32X's PWM, the
Z80 that drives the chips, the PSG — is in `src/`.

SDL2 is the one thing the platform provides rather than the repository.

### macOS

```bash
brew install sdl2 && make run
```

### Windows

Under an MSYS2 UCRT64 shell — the Makefile is a Unix one and `pkg-config`,
`mkdir -p` and the redirects in `check` all want a real shell.

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-pkgconf make git python
```

Then `make run`. The build picks `gcc` and an `.exe` suffix on its own; a
`make CC=clang` still overrides it, and reaches the recompilers too.

Nothing else is Windows-specific. The runtime is standard C plus SDL2 — no
POSIX calls anywhere in `src/` — and the recompiler front ends are Python, so
the playable build needs no cross-toolchain at all. Two things to know: the
`unsigned long` instruction counters in the summary are 32-bit there and wrap
after a few minutes of play, which costs the statistics and nothing else; and
the trace files come out with CRLF endings, which the diff tools read anyway.

Cross-compiling from macOS or Linux works, and is how the Windows build was
checked — Fedora's MinGW packages in a container, which is the least effort
for the most coverage:

```bash
make CC=x86_64-w64-mingw32-gcc CC_BUILD=gcc PKG_CONFIG=x86_64-w64-mingw32-pkg-config
```

`CC_BUILD` is a second compiler because `m68kmake` and `m68kcycles` are not
part of the program: they run *during* the build to generate its sources, so
they belong to the machine doing the building rather than to the target. What
they emit does not depend on which — the cycle table comes out byte for byte
the same from either.

The result is a PE32+ console executable whose only non-system import is
`SDL2.dll`, and it launches. What has *not* been run there is `make check`,
which needs both the reference logs and a cross-toolchain — so the Windows
build is known to start rather than known to agree with the macOS one, and
those are different claims.

Verifying a cross build wants a host that runs x86-64 for real. Wine under
QEMU on an ARM Mac is not one: it aborts inside its own memory manager on a
host/target page-size mismatch, before reaching a single instruction of this.

### Cross-binutils, for `make check` only

`emit --verify` and the recompiler tests assemble what they emitted, which is
what proves the front ends round-trip. `m68k-elf-binutils` comes from Homebrew;
`sh-elf` is built locally because no bottle exists for it.

```bash
brew install m68k-elf-binutils && ./toolchain/build_sh_binutils.sh
```

On Windows both have to be built — the script works under MSYS2, and the same
`--target=m68k-elf` invocation covers the other. `make check` additionally
compares against reference traces in `roms/*.log`, which are not in the
repository.

## Use

```bash
python3 tools/disasm.py info                  # ROM + MARS header summary
python3 tools/disasm.py discover --gaps       # find SH-2 code, print stats
python3 tools/disasm.py tables                # recovered dispatch tables
python3 tools/disasm.py fn 0x060001A0         # disassemble one function
python3 tools/disasm.py dump 0x06000884 40    # raw disassembly at an address
python3 tools/emit_asm.py --verify            # emit SH-2 listing, prove round-trip
python3 tools/validate_decoder.py             # SH-2 decoder vs sh-elf-objdump

python3 tools/disasm68k.py discover           # find 68000 code
python3 tools/disasm68k.py fn 0x3f0           # disassemble one 68000 function
python3 tools/disasm68k.py emit --verify      # emit 68000 listing, prove round-trip
python3 tools/disasm68k.py coverage           # did discovery miss anything it ran?
python3 tools/validate_m68k.py                # 68000 decoder vs m68k-elf-objdump

python3 tools/recompile.py --build            # SH-2 -> C, then compile it
python3 tools/test_recomp.py                  # run recompiled SH-2, check answers
python3 tools/recompile68k.py --build         # 68000 -> C, then compile it
python3 tools/test_recomp68k.py               # recompiled 68000 vs Musashi

make run                                      # play it; recompiled 68000
./build/mars --interp --frames 300            # the same, on the interpreter
./build/mars --frames 600 --wav build/a.wav   # capture what the 32X played
python3 tools/diffpwm.py                      # and hold it to what it played
python3 tools/diffz80.py                      # the sound driver, instruction by
                                              # instruction, against the real Z80
python3 tools/test_psg.py                     # the PSG against its own arithmetic
./build/mars --frames 1800 --sound 4 --wav fm.wav       # 1 PWM, 2 PSG, 4 YM2612
./build/mars --dump-32x build/mars32x.bin     # our frame buffers, palette, regs
python3 tools/refframe.py --ppm f.ppm         # rebuild the real machine's from
python3 tools/refframe.py --compare build/mars32x.bin   # the trace, and compare
python3 tools/refrate.py                      # what an instruction costs, per CPU
```

## How correctness is established

Three independent checks, all of which must pass:

**The decoder** is validated exhaustively. A blob containing all 65,536
instruction words is disassembled with `sh-elf-objdump` and compared word for
word; all 53,752 valid encodings agree, with no disagreements.

The 68000 decoder is checked the same way, but variable-length instructions
cannot be compared position by position — one wrong length and everything after
desynchronises. Each opcode therefore gets its own 12-byte slot padded with
`nop`, so objdump resynchronises at every slot boundary and each candidate is
compared independently, on length as strictly as on text.

**The classification** is validated by round-trip: each listing is reassembled
and diffed against the cartridge.

| | reassembles to |
|---|---|
| SH-2 | 36,864 + 1,024 bytes identical |
| 68000 | 3,145,728 bytes identical (the whole cartridge) |

Both hold for the JU and E images. A single mis-decoded instruction,
mis-rendered operand, or literal pool mistaken for code would break them.

**A third check asks the other way round**: is there code the machine runs that
discovery never found? A reference emulator's instruction log and our own are
both filtered to the addresses the front end has, and anything left over is a
gap. The SH-2 has answered this since its diff existed; the 68000 was first
asked in 2026-08 and came back with 44 addresses, the vertical interrupt handler
among them — reached only through a pointer the engine writes into work RAM at
run time. Both sides are now clean, and `make check` runs all of it.

## Status

The game boots and draws. Both CPUs run against a 32X memory map, the 68000 in
lock step with a reference emulator through its whole boot, and there is a
picture from each half of the machine.

- SH-2 front end **complete**: 247 functions, 1,971 blocks, 15 dispatch tables
- 68000 front end: 4,491 functions, 7,944 blocks, whole-cartridge round-trip,
  and every instruction the gate traces execute is inside it
- SH-2 **recompiler** running: all functions translate to C and compile for
  arm64; 8/8 semantics tests pass on natively executed output
- 68000 **recompiler** is now **the default**: translated code runs the game,
  clean under `-Wall`, 38/38 semantics cases agree with Musashi on values *and*
  condition codes. The interpreter is still built and still needed, for exactly
  the code that does not exist at build time — the adapter's stubs below 0x100
  and the routines the engine assembles into work RAM — which is 291 hand-overs
  in 300 frames. `--interp` puts Musashi back in charge
- **Runtime** runs all three CPUs off one clock, interleaved sixteen times a
  scanline, at rates measured from the reference rather than guessed: 11.0
  cycles an instruction on the 68000, 1.63 on the master SH-2, 1.01 on the
  slave. Output goes to an SDL window and the keyboard drives a six-button pad
- **Against the reference**: 20,078 of the 68000's 20,178 boot instructions
  agree exactly and 53,451 of 54,081 across the whole extract — 98.8% — with no
  divergence control flow does not recover from. The translated 68000 agrees on
  9,025 of 9,848 block entries; both SH-2s walk their extracts the same way
- The 32X half draws **the SEGA logo**, correctly: blue on black with its
  outline, its "TM" and the nebula panels either side
- **Sound**: the 32X's PWM unit is modelled and its output is held to the
  reference's, all 45,227 samples a channel — which are all silence, because
  every audible sound in the opening is the Mega Drive's. An SDL audio device
  now paces the run at the machine's own speed
- **The Z80** runs the sound driver the 68000 uploads into it, and agrees with
  the reference on 32,719 of its 33,984 logged instructions with no divergence
  control flow does not recover from. The 164 bytes it puts into the two sound
  chips are the reference's own, in order
- **All three sound sources play.** Nothing is audible during the SEGA logo —
  the driver mutes the PSG, the 32X's PWM is a constant and the FM is only being
  set up — but past it there is music from every part of the machine at once:
  32X PWM samples, PSG squares, and six channels of FM through Nuked-OPN2.
  `--sound` separates them
- The game runs: SEGA logo, title screen, attract mode, and — played — the save
  and player selects, a level that can be finished, a special stage played to
  its end, the Newtrogic High Zone hub and the Combi Catcher. 200,000 frames
  headless with no stall
- **A recorded play session is a gate.** `--record` writes the pad back out in
  our own frame numbering, so a session played once replays for ever;
  `tools/test_session.py` is the last step of `make check` and the only gate
  that reaches past the reference logs' 1.7 seconds
- Five faults that only playing could find are fixed: the 32X's priority is per
  dot rather than per screen, a byte write to a comm register is a byte (which
  is the special stage's sound effects), the Mega Drive's shadow/highlight mode
  is modelled, the cartridge's battery is saved to `<cartridge>.sav`, and the
  SH-2 recompiler writes PR before a call's delay slot rather than after (which
  is the special stage's character on its run to the Chaos Ring)
- Next: the two 68000 backends against each other, which is the one oracle in
  hand that the reference logs cannot be

See [docs/state.md](docs/state.md) for where things stand — numbers, flags and
open items, no prose — [docs/architecture.md](docs/architecture.md) for
findings, and [docs/roadmap.md](docs/roadmap.md) for how each of them was
arrived at and what was tried and rejected.

The headline finding: the SH-2 program is only 36 KB. This is a 68000
Sonic-engine game that uses the 32X as a video co-processor, so the 68000 is the
main target and the SH-2 side is a small appendix.

## License

This repository's own code — the runtime in `src/`, the analysis and
recompilation pipeline in `tools/`, the tests and the docs — is MIT, in
[LICENSE](LICENSE).

The submodules keep their own terms: Musashi is MIT, and Nuked-OPN2 is
LGPL 2.1-or-later, which is a condition on distributing a *binary* that links
it rather than on anything here. SDL2 is zlib.

None of this licenses the game. *Knuckles' Chaotix* is Sega's, no cartridge
data is in the repository, and nothing here runs without a dump you provide.
The recompiler's output is derived from that dump and is written to `build/`,
which is ignored for the same reason `roms/` is: what the pipeline produces
from a copyrighted ROM is not this project's to hand out.
