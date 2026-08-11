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
37,888 bytes total (a 36 KB SDRAM image plus a 1 KB overlay). Knuckles' Chaotix
is a 68000 Sonic-engine game that uses the 32X as a video co-processor, not an
SH-2 game. The 68000 is therefore the primary decompilation target; the SH-2
side is a comparatively small, tractable appendix.

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

## The cache-array overlay

The SH-2 program is not only the SDRAM image. The slave's init routine at
`0x06000284` copies 0x100 longs from cartridge offset `0x07FC00` into
`0xC0000000` — the SH-2 **cache data array** — and then calls into it. Running
from the cache array avoids cartridge wait states, and the 32X allows the cache
to be used as directly addressable RAM.

This matters for the address model: the SH-2 encodes cache behaviour in the top
address bits, but only `0x00000000` (cached) and `0x20000000` (cache-through)
are mirrors of each other. `0x60000000` (cache address array) and `0xC0000000`
(cache data array) are distinct storage. Collapsing all of them — as a naive
`addr & 0x1FFFFFFF` does — aliases this routine onto the boot ROM.

The overlay has its own header of `bra` stubs, one per entry point, mirroring
the SDRAM image's layout at `0x060001A0`. Its tail is `0xAAAA`/`0xFFFF` fill.

## The sound path

Two halves, and they belong to different processors.

**The 32X's own** is PWM, and the whole of it is that cache-array overlay. The
slave programs the unit from `0xC0000008` — cycle `0x417`, control `0x0105`, so
TM is 1 — and then does nothing but take the resulting timer interrupt, which is
its only clock. The handler enters at `0xC0000004`; every second one mixes four
channels of 8-bit PCM at `0xC000012C` and pushes two stereo pairs into the
sample FIFOs at `0x20004034` and `0x20004036`. A channel's state is a 16-byte
table entry — start, length, loop, rate — and its volumes come out of the high
byte of the command word the master writes.

| register | |
|---|---|
| `0x20004030` | control: TM in bits 11-8, RMD in 3-2, LMD in 1-0 |
| `0x20004032` | cycle; one PWM cycle is `cycle - 1` SH-2 cycles |
| `0x20004034` / `36` / `38` | left / right / mono FIFO, three words deep |

A word written to a FIFO is a pulse width, so the cycle's midpoint is silence;
this driver centres on `0x200` against a midpoint of 523. A read of the same
address gives bit 15 full, bit 14 empty. The 68000 sees the same block at
`0xA15130`, and uses it only to clear the unit during its 32X init.

**The Mega Drive's** is a Z80 driver in the 8 KB at `0xA00000`, uploaded by the
68000 and running under 4 KB of code. It writes the YM2612 through both port
pairs — `$4000`/`$4001` for channels 1-3, `$4002`/`$4003` for 4-6 — and the PSG
at `$7F11`, and it banks the 68000 window through `$6000` to reach its data in
the cartridge. The 68000 also writes the PSG directly at `0xC00011`. Every
audible sound in the opening is here: over the whole reference extract the 32X's
PWM output is a constant `0x200`.

## Dispatch idioms

Three forms account for essentially all table-driven control flow, and
recognising them is what makes discovery work at all:

| | Shape | Entries |
|---|---|---|
| A | `mova Lbase,r0` ; `mov.l @(r0,rM),rN` ; `jmp @rN` | 32-bit absolute addresses |
| B | `mova Lbase,r0` ; `mov.w @(r0,rM),rN` ; `braf rN` | 16-bit offsets from PC+4 |
| C | `mov.l Lp,rB` ; `mov.b @(r0,rB),rT` ; `braf rT` | 8-bit offsets from PC+4 |

In A and B the `mova` names the table exactly. In C the literal is an indexing
*origin* that sits before the real entries, so the table is instead located
immediately after the branch's delay slot, and it is bounded by the lowest
target it points at — a table cannot extend into the code it dispatches to.

Note also that `bsrf`/`braf` compute `PC + 4 + Rn` rather than jumping to `Rn`.
That is the standard far-call form, so treating the register as an absolute
address stops discovery at the very first one.

## Code discovery status (SH-2)

`python3 tools/disasm.py discover`

```
functions       : 208
basic blocks    : 1,772
instructions    : 9,243    (18,486 bytes)
data (pool+tbl) : 2,448 bytes
dispatch tables : 15 recovered
unresolved      : 8 indirect transfers
SDRAM blob      : 55.4% classified;  overlay: 488/1,024 bytes
```

Of the 16,432 unclassified bytes in the SDRAM blob, only ~190 still decode as
plausible code; the rest are data tables and buffers. The blob is a mixed
code+data image, so full byte coverage is not the goal — the goal is that every
byte is either understood or provably not code.

The 8 remaining unresolved transfers are genuine runtime function pointers
(`jsr @r12`, `jsr @r14`), where the callee is chosen by the caller at run time.
Resolving them needs interprocedural dataflow, not better local pattern
matching.

## Verification

`python3 tools/emit_asm.py --verify` emits a full listing and reassembles it
with `sh-elf-as`:

```
sh2_sdram             36,864 bytes identical
sh2_overlay_c0000000   1,024 bytes identical
```

Both hold for the JU and E images. This is the front end's correctness gate: if
a single instruction were decoded wrongly, an operand mis-rendered, or a literal
pool mistaken for code, the bytes would not match.

One caveat the round-trip cannot catch: bytes are bytes, so misclassifying data
*as* code still reassembles. Recursive descent does walk into literal pools via
architecturally-valid but semantically unreachable fallthrough edges — at
`0x0600540E` a `bf` falls into a pool, because the preceding `bt.s` already
consumed the T-set case. Data proven by an instruction reference therefore
outranks a code claim that arrived only by fallthrough.

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
