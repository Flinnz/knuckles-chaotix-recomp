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
python3 tools/validate_m68k.py                # 68000 decoder vs m68k-elf-objdump

python3 tools/recompile.py --build            # SH-2 -> C, then compile it
python3 tools/test_recomp.py                  # run recompiled SH-2, check answers
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

Front ends are complete and the SH-2 half runs natively against a 32X memory
map. The game does not boot: the 68000, which is the actual engine, is not
implemented yet.

- SH-2 front end **complete**: 208 functions, 1,772 blocks, 15 dispatch tables
- 68000 front end **complete**: 368 functions, 2,957 blocks, whole-ROM round-trip
- SH-2 **recompiler** running: all 208 functions translate to C and compile for
  arm64; 8/8 semantics tests pass on natively executed output
- **Runtime** runs both CPUs: Musashi drives the 68000, the recompiled code
  drives the master SH-2, output goes to an SDL window. The 68000 boots, clears
  the 32X adapter checks and takes control of the 32X VDP, with 9 unmapped
  accesses across a 600-frame run
- **Blocked on the SH-2 ready handshake**: the 68000 spins waiting for `"M_OK"`
  and `"S_OK"` in the comm registers, which the master and slave SH-2s post when
  their init completes. The slave is not running yet
- 50 indirect transfers unresolved across both CPUs, mostly runtime function
  pointers needing interprocedural dataflow
- Next: follow the engine's data-driven tables to raise 68000 coverage, then
  start the recompiler

See [docs/architecture.md](docs/architecture.md) for findings and
[docs/roadmap.md](docs/roadmap.md) for the plan.

The headline finding: the SH-2 program is only 36 KB. This is a 68000
Sonic-engine game that uses the 32X as a video co-processor, so the 68000 is the
main target and the SH-2 side is a small appendix.
