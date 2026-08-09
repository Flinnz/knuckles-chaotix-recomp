#!/usr/bin/env python3
"""Discover and disassemble the SH-2 code in a 32X cartridge.

  python3 tools/disasm.py info                 ROM + MARS header summary
  python3 tools/disasm.py discover             run code discovery, print stats
  python3 tools/disasm.py fn <addr>            disassemble one function
  python3 tools/disasm.py dump <addr> <count>  raw disassembly at an address
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mars                                    # noqa: E402
from sh2.analyze import Analyzer, Image        # noqa: E402
from sh2.decode import decode                  # noqa: E402

DEFAULT_ROM = "roms/Knuckles' Chaotix (JU) (32X) [!].32x"


def build(rom_path):
    rom = mars.Rom(rom_path)
    h = rom.mars_header()
    img = Image(norm=mars.sh2_phys)
    # The SDRAM image is copied out of ROM by the 32X boot code before the
    # master SH-2 starts, so at runtime it is live memory, not cartridge.
    img.add(mars.SDRAM_BASE + h.sh2_dst, rom.sdram_image())
    # The cartridge itself stays mapped and most code executes in place.
    img.add(mars.ROM_BASE, rom.data)

    sdram_lo = mars.SDRAM_BASE + h.sh2_dst
    sdram_hi = sdram_lo + h.sh2_size
    rom_hi = mars.ROM_BASE + len(rom.data)

    def is_code(a):
        if a & 1:
            return False
        a = mars.sh2_phys(a)
        return (sdram_lo <= a < sdram_hi) or (mars.ROM_BASE <= a < rom_hi)

    return rom, h, img, Analyzer(img, is_code_addr=is_code)


def seed(rom, h, az):
    """Seed discovery with every architecturally-defined entry point."""
    az.add_entry(h.master_start, "master reset")
    az.add_entry(h.slave_start, "slave reset")
    for label, vbr in (("master", h.master_vbr), ("slave", h.slave_vbr)):
        for i in range(32):
            a = vbr + i * 4
            if az.img.readable(a, 4):
                az.add_entry(az.img.u32(a), f"{label} vector {i}")


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
    print(f"SH-2 image     : ROM 0x{h.sh2_src:06X} -> SDRAM 0x{mars.SDRAM_BASE + h.sh2_dst:08X}"
          f"  ({h.sh2_size:,} bytes)")
    print(f"master entry   : 0x{h.master_start:08X}   VBR 0x{h.master_vbr:08X}")
    print(f"slave  entry   : 0x{h.slave_start:08X}   VBR 0x{h.slave_vbr:08X}")
    print()
    print(f"68000 reset PC : 0x{rom.u32(4):08X}")
    print(f"68000 SSP      : 0x{rom.u32(0):08X}")


def cmd_discover(args):
    rom, h, img, az = build(args.rom)
    seed(rom, h, az)
    az.run()
    s = az.stats()

    in_sdram = sum(1 for a in az.funcs if mars.sh2_phys(a) >= mars.SDRAM_BASE)
    in_rom = s["functions"] - in_sdram
    code_rom = sum(1 for a in az.code if mars.sh2_phys(a) < mars.SDRAM_BASE)

    print(f"functions found : {s['functions']:,}  "
          f"({in_sdram:,} in SDRAM, {in_rom:,} in ROM)")
    print(f"instructions    : {s['code_insns']:,}  ({s['code_bytes']:,} bytes)")
    print(f"  in cart ROM   : {code_rom:,} insns ({code_rom * 2:,} bytes)")
    print(f"literal pool    : {s['pool_bytes']:,} bytes")
    print(f"unresolved jumps: {s['unresolved']:,}  (jump tables / computed targets)")
    print(f"ROM coverage    : {100.0 * (code_rom * 2 + s['pool_bytes']) / len(rom.data):.1f}%"
          f" of {len(rom.data) // 1024} KB")

    big = sorted(az.funcs.values(), key=lambda f: -f.size())[:15]
    print("\nlargest functions:")
    for f in big:
        print(f"  0x{f.start:08X}  {f.size():6,} bytes  "
              f"{len(f.blocks):4} blocks  {len(f.callees):3} callees")

    if args.out:
        with open(args.out, "w") as fh:
            for a in sorted(az.funcs):
                f = az.funcs[a]
                fh.write(f"0x{a:08X}\t{f.size()}\t{len(f.blocks)}\t"
                         f"{len(az.xrefs.get(a, ()))}\n")
        print(f"\nwrote function list -> {args.out}")


def _disasm_lines(img, az, start, end):
    addr = start
    out = []
    while addr < end:
        if addr in az.data:
            out.append(f"  {addr:08X}:  {img.u32(addr) if addr % 4 == 0 else img.u16(addr):08X}"
                       f"    .long")
            addr += 4
            continue
        if not img.readable(addr, 2):
            break
        ins = decode(img.u16(addr), addr)
        note = ""
        if ins.pool and img.readable(ins.pool[0], ins.pool[1]):
            v = img.u32(ins.pool[0]) if ins.pool[1] == 4 else img.s16(ins.pool[0])
            note = f"   ! 0x{v & 0xFFFFFFFF:08X}"
        out.append(f"  {addr:08X}:  {ins.word:04X}      {ins.text()}{note}")
        addr += 2
    return out


def cmd_fn(args):
    rom, h, img, az = build(args.rom)
    seed(rom, h, az)
    az.run()
    a = int(args.addr, 0)
    fn = az.funcs.get(a)
    if not fn:
        print(f"no discovered function at 0x{a:08X}; use `dump` for a raw view")
        return 1
    print(f"function 0x{fn.start:08X}  {fn.size()} bytes, {len(fn.blocks)} blocks")
    if az.xrefs.get(a):
        print(f"  called from: " + ", ".join(f"0x{x:08X}" for x in sorted(az.xrefs[a])[:8]))
    for b in sorted(fn.blocks.values(), key=lambda b: b.start):
        print(f"\n .block 0x{b.start:08X}")
        for ins in b.insns:
            note = ""
            if ins.pool and img.readable(ins.pool[0], ins.pool[1]):
                v = img.u32(ins.pool[0]) if ins.pool[1] == 4 else img.s16(ins.pool[0])
                note = f"   ! 0x{v & 0xFFFFFFFF:08X}"
            print(f"  {ins.addr:08X}:  {ins.word:04X}      {ins.text()}{note}")
    if fn.unresolved:
        print("\n unresolved indirect transfers:")
        for addr, kind in fn.unresolved:
            print(f"  0x{addr:08X}  {kind}")
    return 0


def cmd_dump(args):
    rom, h, img, az = build(args.rom)
    a = int(args.addr, 0)
    for line in _disasm_lines(img, az, a, a + int(args.count) * 2):
        print(line)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--rom", default=DEFAULT_ROM)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    d = sub.add_parser("discover")
    d.add_argument("--out", help="write discovered function list to this file")
    f = sub.add_parser("fn")
    f.add_argument("addr")
    u = sub.add_parser("dump")
    u.add_argument("addr")
    u.add_argument("count", nargs="?", default=32)
    args = p.parse_args()
    return {"info": cmd_info, "discover": cmd_discover,
            "fn": cmd_fn, "dump": cmd_dump}[args.cmd](args) or 0


if __name__ == "__main__":
    sys.exit(main())
