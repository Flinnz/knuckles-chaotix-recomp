# chaotix

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
docs/             architecture findings and roadmap
```

## Setup

Third-party dependencies (not vendored):

```bash
brew install sdl2
git clone --depth 1 https://github.com/kstenerud/Musashi.git third_party/musashi
make run
```

Musashi (MIT, Karl Stenerud) provides the 68000 interpreter; `src/m68kconf.h`
overrides its config down to a bare 68000 so the vendored copy stays untouched.


`m68k-elf-binutils` comes from Homebrew; `sh-elf` is built locally because no
bottle exists for it.

```bash
brew install m68k-elf-binutils && ./toolchain/build_sh_binutils.sh
```

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
- 68000 front end: 561 functions, 7,943 blocks, whole-cartridge round-trip,
  and every instruction the gate traces execute is inside it
- SH-2 **recompiler** running: all functions translate to C and compile for
  arm64; 8/8 semantics tests pass on natively executed output
- 68000 **recompiler** translating: all 561 functions to C, clean under `-Wall`,
  with 38/38 semantics cases agreeing with Musashi on values *and* condition
  codes. Not yet swapped in for the interpreter
- **Runtime** runs both SH-2s and the 68000: Musashi interprets the 68000, the
  recompiled code runs the master and slave SH-2s, output goes to an SDL window
  and the keyboard drives a six-button pad
- **Against the reference**: 20,072 of the 68000's 20,178 boot instructions
  agree exactly, and 52,381 of 54,081 across the whole extract, with no
  divergence control flow does not recover from. Both SH-2s walk their extracts
  the same way
- Next: interleave the two CPUs — the SH-2 currently runs inside the 68000's
  register writes, so every rendezvous answers on the first poll, which is 380
  of the 452 differences left — and run the game on the recompiled 68000

See [docs/architecture.md](docs/architecture.md) for findings and
[docs/roadmap.md](docs/roadmap.md) for the plan.

The headline finding: the SH-2 program is only 36 KB. This is a 68000
Sonic-engine game that uses the 32X as a video co-processor, so the 68000 is the
main target and the SH-2 side is a small appendix.
