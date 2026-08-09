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
python3 tools/emit_asm.py --verify            # emit listing, prove it round-trips
python3 tools/validate_decoder.py             # decoder vs sh-elf-objdump
```

## How correctness is established

Two independent checks, both of which must pass:

**The decoder** is validated exhaustively. A blob containing all 65,536
instruction words is disassembled with `sh-elf-objdump` and compared word for
word; all 53,752 valid encodings agree, with no disagreements.

**The classification** is validated by round-trip. `emit_asm.py` writes a full
listing and reassembles it with `sh-elf-as`; the result is byte-identical to the
cartridge (36,864 + 1,024 bytes, on both the JU and E images). A single
mis-decoded instruction, mis-rendered operand, or literal pool mistaken for code
would break it.

## Status

Front end only — nothing is recompiled or runnable yet.

- SH-2 front end **complete**: 208 functions, 1,772 basic blocks, 15 dispatch
  tables, byte-exact round-trip
- 8 indirect transfers unresolved — all runtime function pointers, needing
  interprocedural dataflow
- 68000 boot path identified; 68000 front end not started

See [docs/architecture.md](docs/architecture.md) for findings and
[docs/roadmap.md](docs/roadmap.md) for the plan.

The headline finding: the SH-2 program is only 36 KB. This is a 68000
Sonic-engine game that uses the 32X as a video co-processor, so the 68000 is the
main target and the SH-2 side is a small appendix.
