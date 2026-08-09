# Knuckles' Chaotix — hardware and ROM findings

Everything here was derived from the cartridge images in `roms/` using the tools
in `tools/`. Reproduce any of it with `python3 tools/disasm.py info|discover`.

## Cartridge images

| Region | SHA-1 | Header checksum |
|---|---|---|
| JU | `0c2fff7bc79ed26507c08ac47464c3af19f7ced7` | `B61C` stored = computed |
| E  | `5c1a2e327a656217604d4bae7e141764a7e59922` | `85FC` stored = computed |

Both are 3,145,728 bytes (3 MB), title `CHAOTIX`, serial `GM MK-84503-00`,
`(C)SEGA 1995.FEB`. Both verify against their own header checksum, so the dumps
are clean. Work is done against the JU image; E differs by region byte and
localisation data.

## The four processors

The 32X is a Mega Drive with an add-on board, and a game runs on all of it:

| CPU | Clock | Role in this game |
|---|---|---|
| Motorola 68000 | 7.6 MHz | **The game itself** — engine, objects, level logic, sound driver |
| SH-2 master | 23 MHz | 32X-side command loop, driven by the 68000 |
| SH-2 slave | 23 MHz | Second renderer/worker, same command-loop shape |
| Z80 | 3.58 MHz | Sound sub-CPU |

**The single most important structural finding:** the SH-2 program is only
36,864 bytes total. Knuckles' Chaotix is a 68000 Sonic-engine game that uses the
32X as a video co-processor, not an SH-2 game. The 68000 is therefore the
primary decompilation target; the SH-2 side is a comparatively small, tractable
appendix.

## MARS header (cartridge offset 0x3C0)

```
module name    'MARS CHECK MODE'
SH-2 image     ROM 0x077800 -> SDRAM 0x06000000, 36,864 bytes
master entry   0x060001A0    master VBR 0x06000000
slave  entry   0x060001A4    slave  VBR 0x06000080
68000 reset PC 0x000003F0    68000 SSP  0x00FF0200
```

Both SH-2 vector tables are 32 entries. Vector 0/2 are reset PC, 1/3 are reset
SP (`0x06040000`, the top of the 256 KB SDRAM). Every unused vector points at
`0x060001A8`, which is `bra` to itself — a deliberate hang on a stray interrupt.

## Address maps

### SH-2

The SH-2 encodes cache behaviour in the top three address bits, so each region
appears at several addresses (`0x02000000` cached, `0x22000000` cache-through,
etc.). `tools/mars.py:sh2_phys` collapses these.

| Range | Contents |
|---|---|
| `0x00000000` | per-CPU boot ROM |
| `0x00004000` | 32X system registers (comm ports, interrupt control) |
| `0x00004100` | 32X VDP registers |
| `0x00004200` | palette, 512 bytes |
| `0x02000000` | cartridge, 4 MB window |
| `0x04000000` | frame buffer, 128 KB (+ overwrite image at `0x04020000`) |
| `0x06000000` | SDRAM, 256 KB — **all SH-2 code runs here** |
| `0xFFFFF000` | on-chip peripherals (DMAC, FRT, INTC, WDT) |

### 68000 (with the adapter enabled)

| Range | Contents |
|---|---|
| `0x840000` | 32X frame buffer window (+ overwrite at `0x860000`) |
| `0x880000` | first 512 KB of cartridge, fixed |
| `0x900000` | 1 MB banked cartridge window |
| `0xA00000` | Z80 space |
| `0xA10000` | controller / version I/O |
| `0xA15100` | 32X system + PWM registers |
| `0xC00000` | Genesis VDP |
| `0xFF0000` | 64 KB work RAM |

The 68000 vector table confirms this: exception vectors point at `0x00880B2E`,
i.e. cartridge offset `0xB2E` seen through the `0x880000` window. Vectors 9–11
point into `0xFFFFC0xx`, a dispatch table the game builds in work RAM.

## Boot sequence

**68000** (`0x3F0`) does the standard 32X handshake: masks interrupts, checks for
the literal `"MARS"` signature at `0xA130EC`, waits on the adapter-control bit at
`0xA15101`, then proceeds into game init.

**SH-2 master** (`0x060001A0`) branches to `0x06000884`, which programs the
free-running timer at `0xFFFFFE10`, sets VBR/SP, points `r14` at the system
registers (`0x20004000`), waits on a comm register, then spins until the 68000
writes `"REDY"` (`0x52454459`) to `0x06003610`. It then enters a command loop:
poll comm register `0x20004020`, look the command up in a table, and dispatch.

**SH-2 slave** (`0x060001A4`) is the same shape, reading its command byte from
`0x20004005` and dispatching through a table of 16-bit offsets.

So the control relationship is: **the 68000 drives; the SH-2s serve commands.**

## Code discovery status (SH-2)

`python3 tools/disasm.py discover`

```
functions found : 193      (all in SDRAM; none executed from cartridge)
instructions    : 8,744    (17,488 bytes)
literal pool    : 1,758 bytes
SDRAM blob      : 52.2% classified as code or literal pool
jump tables     : 14 recovered
unresolved      : 9 indirect transfers
```

No SH-2 code is executed from the cartridge window — the 36 KB SDRAM image is
the whole SH-2 program. The uncovered ~48% of the blob is a mix of data tables
and uninitialised scratch space; the two largest gaps (`0x06005FD4`, 5,628 bytes
and `0x06007DA0`, 4,704 bytes) sit where buffers would be expected.

## ROM layout

A 16 KB-block entropy scan (`~7.5+` = compressed, `~5` with high zero-fill =
code) shows the cartridge is dominated by compressed art in the classic
Sonic-engine style, with 68000 code concentrated below `0x200000`:

```
000000-00BFFF  data / 68000 code
00C000-00FFFF  code
010000-01FFFF  data + code
020000-07FFFF  data                     (SH-2 image lives at 0x077800)
080000-09FFFF  code + data
0A0000-11FFFF  data
120000-16FFFF  data
170000-2BFFFF  mostly compressed art, interleaved with tables
2C0000-2FBFFF  compressed art + data
2FC000-2FFFFF  zero padding
```
