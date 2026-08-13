#!/usr/bin/env python3
"""Discover and disassemble the SH-2 code in a 32X cartridge.

  python3 tools/disasm.py info                 ROM + MARS header summary
  python3 tools/disasm.py discover             run code discovery, print stats
  python3 tools/disasm.py fn <addr>            disassemble one function
  python3 tools/disasm.py dump <addr> <count>  raw disassembly at an address
  python3 tools/disasm.py tables               list recovered dispatch tables
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mars                                    # noqa: E402
from sh2.analyze import Analyzer, Image        # noqa: E402
from sh2.decode import decode                  # noqa: E402

DEFAULT_ROM = "roms/Knuckles' Chaotix (JU) (32X) [!].32x"

# Code the game copies out of the cartridge into somewhere else before running
# it, as (destination, rom_offset, size). The SH-2 cache data array at
# 0xC0000000 is real storage, and the slave's init routine at 0x06000284 copies
# 0x100 longs there from ROM 0x07FC00 and then calls into it — running from the
# cache array avoids cartridge wait states.
OVERLAYS = [
    (0xC0000000, 0x07FC00, 0x400, "cache data array routine"),
]


def build(rom_path):
    rom = mars.Rom(rom_path)
    h = rom.mars_header()
    img = Image(norm=mars.sh2_phys)
    # The SDRAM image is copied out of ROM by the boot code before the master
    # SH-2 starts, so at runtime it is live memory, not cartridge.
    img.add(mars.SDRAM_BASE + h.sh2_dst, rom.sdram_image())
    img.add(mars.ROM_BASE, rom.data)
    for dest, off, size, _ in OVERLAYS:
        img.add(dest, rom.data[off:off + size])

    sdram_lo = mars.SDRAM_BASE + h.sh2_dst
    sdram_hi = sdram_lo + h.sh2_size
    rom_hi = mars.ROM_BASE + len(rom.data)
    ranges = [(sdram_lo, sdram_hi), (mars.ROM_BASE, rom_hi)]
    ranges += [(d, d + s) for d, _, s, _ in OVERLAYS]

    def is_code(a):
        if a & 1:
            return False
        a = mars.sh2_phys(a)
        return any(lo <= a < hi for lo, hi in ranges)

    return rom, h, img, Analyzer(img, is_code_addr=is_code)


def seed(rom, h, az):
    """Seed discovery with every architecturally-defined entry point."""
    az.add_function(h.master_start, "master reset")
    az.add_function(h.slave_start, "slave reset")
    # Both vector tables are pointer arrays, not code — mark them so the
    # emitter reproduces them as .long rather than trying to disassemble them.
    for label, vbr in (("master", h.master_vbr), ("slave", h.slave_vbr)):
        for i in range(32):
            a = vbr + i * 4
            if az.img.readable(a, 4):
                az.mark_data(a, "long", 4)
                az.add_function(az.img.u32(a), f"{label} vector {i}")
    for dest, _, size, why in OVERLAYS:
        # Copied blocks start with a header of `bra` stubs, one per entry
        # point, exactly like the SDRAM image does at 0x060001A0. Seed each of
        # them rather than only the first.
        a = dest
        for i in range(8):
            if not az.img.readable(a, 2):
                break
            ins = decode(az.img.u16(a), a)
            if ins.mnem != "bra":
                break
            az.add_function(a, f"{why} entry {i}")
            a += 4
        az.add_function(dest, why)


def analyse(rom_path, scan_tables=True):
    rom, h, img, az = build(rom_path)
    seed(rom, h, az)
    az.run()
    if scan_tables:
        # Handler tables living in data are invisible to the mova idioms, so
        # sweep for them and re-run until nothing new turns up.
        sweep = [(mars.SDRAM_BASE + h.sh2_dst, h.sh2_size)]
        sweep += [(d, s_) for d, _, s_, _ in OVERLAYS]
        def converge(with_self_relative):
            for _ in range(8):
                found = sum(az.scan_pointer_tables(lo, lo + size)
                            for lo, size in sweep)
                found += az.scan_after_returns()
                if with_self_relative:
                    found += sum(az.scan_self_relative(lo, lo + size)
                                 for lo, size in sweep)
                if not found:
                    return
                az.run()

        # The self-relative sweep runs only once the others have converged, and
        # the order is load-bearing. `scan_after_returns` skips an address that
        # is already code, and it needs more than one round to reach some of
        # them — 0x06004A24 is seeded on the second. Sweeping in the same round
        # claimed those bytes as fall-through first, so the address stayed code
        # and stopped being an *entry*, which is the one thing an indirect
        # transfer to it needs. That cost the attract loop its 200,000 frames
        # and showed up as "no recompiled block at 0x06004A24" at frame 688.
        converge(False)
        converge(True)
    return rom, h, img, az


def cmd_info(args):
    rom, h, img, az = build(args.rom)
    stored, computed = rom.genesis_checksum()
    print(f"file           : {os.path.basename(rom.path)}")
    print(f"size           : {len(rom.data):,} bytes ({len(rom.data) // 1024} KB)")
    print(f"title          : {rom.title}")
    print(f"serial         : {rom.serial}   region: {rom.region}")
    print(f"checksum       : stored {stored:04X}, computed {computed:04X} "
          f"({'OK' if stored == computed else 'MISMATCH'})")
    print()
    print(f"MARS module    : {h.module_name!r}")
    print(f"SH-2 image     : ROM 0x{h.sh2_src:06X} -> SDRAM "
          f"0x{mars.SDRAM_BASE + h.sh2_dst:08X}  ({h.sh2_size:,} bytes)")
    print(f"master entry   : 0x{h.master_start:08X}   VBR 0x{h.master_vbr:08X}")
    print(f"slave  entry   : 0x{h.slave_start:08X}   VBR 0x{h.slave_vbr:08X}")
    print(f"68000 reset PC : 0x{rom.u32(4):08X}   SSP 0x{rom.u32(0):08X}")
    for dest, off, size, why in OVERLAYS:
        print(f"overlay        : ROM 0x{off:06X} -> 0x{dest:08X} "
              f"({size} bytes, {why})")


def cmd_discover(args):
    rom, h, img, az = analyse(args.rom)
    s = az.stats()
    lo = mars.SDRAM_BASE + h.sh2_dst
    hi = lo + h.sh2_size

    classified = sum(1 for a in az.code if lo <= a < hi) * 2
    classified += sum(1 for a in az.data if lo <= a < hi)
    print(f"functions       : {s['functions']:,}")
    print(f"basic blocks    : {s['blocks']:,}")
    print(f"instructions    : {s['code_insns']:,}  ({s['code_bytes']:,} bytes)")
    print(f"data (pool+tbl) : {s['data_bytes']:,} bytes")
    print(f"dispatch tables : {s['tables']:,}")
    print(f"unresolved      : {s['unresolved']:,} indirect transfers")
    print(f"SDRAM blob      : {classified:,}/{h.sh2_size:,} bytes classified "
          f"({100.0 * classified / h.sh2_size:.1f}%)")

    for dest, _, size, why in OVERLAYS:
        c = sum(1 for a in az.code if dest <= a < dest + size) * 2
        c += sum(1 for a in az.data if dest <= a < dest + size)
        print(f"overlay 0x{dest:08X}: {c:,}/{size:,} bytes classified")

    if args.gaps:
        print("\nlargest unclassified runs in the SDRAM blob:")
        runs, run = [], None
        for a in range(lo, hi):
            known = (a in az.code) or (a - 1 in az.code) or (a in az.data)
            if not known:
                run = (a, a) if run is None else (run[0], a)
            elif run:
                runs.append(run)
                run = None
        if run:
            runs.append(run)
        for g in sorted(runs, key=lambda g: -(g[1] - g[0]))[:12]:
            print(f"  0x{g[0]:08X}-0x{g[1]:08X}  {g[1] - g[0] + 1:,} bytes")

    big = sorted(az.funcs.values(), key=lambda f: -f.size())[:12]
    print("\nlargest functions:")
    for f in big:
        print(f"  0x{f.start:08X}  {f.size():6,} bytes  {len(f.blocks):4} blocks  "
              f"{len(az.xrefs.get(f.start, ())):3} xrefs")


def cmd_tables(args):
    rom, h, img, az = analyse(args.rom)
    print(f"{len(az.tables)} dispatch tables\n")
    for a in sorted(az.tables):
        t = az.tables[a]
        print(f"  dispatch 0x{a:08X}  {t.kind:4} table at 0x{t.base:08X}, "
              f"{len(t.entries)} entries")
        for ea, tgt in t.entries[:6]:
            print(f"      [0x{ea:08X}] -> 0x{tgt:08X}")
        if len(t.entries) > 6:
            print(f"      ... {len(t.entries) - 6} more")


def _line(img, ins):
    note = ""
    if ins.pool and img.readable(ins.pool[0], ins.pool[1]):
        v = img.u32(ins.pool[0]) if ins.pool[1] == 4 else img.s16(ins.pool[0])
        note = f"   ! 0x{v & 0xFFFFFFFF:08X}"
    return f"  {ins.addr:08X}:  {ins.word:04X}      {ins.text()}{note}"


def cmd_fn(args):
    rom, h, img, az = analyse(args.rom)
    a = int(args.addr, 0)
    fn = az.funcs.get(a)
    if not fn:
        print(f"no discovered function at 0x{a:08X}; try `dump`")
        return 1
    why = az.entry_reasons.get(a, "")
    print(f"function 0x{fn.start:08X}  {fn.size()} bytes, {len(fn.blocks)} blocks"
          f"{'  [' + why + ']' if why else ''}")
    if az.xrefs.get(a):
        refs = sorted(az.xrefs[a])
        print("  xrefs: " + ", ".join(f"0x{x:08X}" for x in refs[:8])
              + (f" (+{len(refs) - 8})" if len(refs) > 8 else ""))
    for b in sorted(fn.blocks.values(), key=lambda b: b.start):
        print(f"\n .block 0x{b.start:08X}")
        for ins in b.insns:
            print(_line(img, ins))
    if fn.unresolved:
        print("\n unresolved indirect transfers:")
        for addr, kind in fn.unresolved:
            print(f"  0x{addr:08X}  {kind}")
    return 0


def cmd_dump(args):
    rom, h, img, az = build(args.rom)
    a = int(args.addr, 0)
    for _ in range(int(args.count)):
        if not img.readable(a, 2):
            break
        print(_line(img, decode(img.u16(a), a)))
        a += 2


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--rom", default=DEFAULT_ROM)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    d = sub.add_parser("discover")
    d.add_argument("--gaps", action="store_true", help="list unclassified runs")
    sub.add_parser("tables")
    f = sub.add_parser("fn")
    f.add_argument("addr")
    u = sub.add_parser("dump")
    u.add_argument("addr")
    u.add_argument("count", nargs="?", default=32)
    args = p.parse_args()
    return {"info": cmd_info, "discover": cmd_discover, "tables": cmd_tables,
            "fn": cmd_fn, "dump": cmd_dump}[args.cmd](args) or 0


if __name__ == "__main__":
    sys.exit(main())
