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
python3 tools/emit_asm.py --verify            # emit SH-2 listing, prove round-trip
python3 tools/validate_decoder.py             # SH-2 decoder vs sh-elf-objdump

python3 tools/disasm68k.py discover           # find 68000 code
python3 tools/disasm68k.py fn 0x3f0           # disassemble one 68000 function
python3 tools/disasm68k.py emit --verify      # emit 68000 listing, prove round-trip
python3 tools/validate_m68k.py                # 68000 decoder vs m68k-elf-objdump
```

## How correctness is established

Two independent checks, both of which must pass:

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

## Status

Front ends only — nothing is recompiled or runnable yet.

- SH-2 **complete**: 208 functions, 1,772 blocks, 15 dispatch tables
- 68000 **complete**: 368 functions, 2,957 blocks, whole-ROM round-trip
- 50 indirect transfers unresolved across both CPUs, mostly runtime function
  pointers needing interprocedural dataflow
- Next: follow the engine's data-driven tables to raise 68000 coverage, then
  start the recompiler

See [docs/architecture.md](docs/architecture.md) for findings and
[docs/roadmap.md](docs/roadmap.md) for the plan.

The headline finding: the SH-2 program is only 36 KB. This is a 68000
Sonic-engine game that uses the 32X as a video co-processor, so the 68000 is the
main target and the SH-2 side is a small appendix.
