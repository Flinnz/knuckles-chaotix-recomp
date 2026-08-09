"""Sega 32X ("Mars") cartridge and address-space model.

The 32X is four processors sharing one cartridge:
  * 2x Hitachi SH-2 @ 23 MHz (master + slave) — the 32X add-on
  * Motorola 68000 @ 7.6 MHz — the host Mega Drive
  * Zilog Z80 @ 3.58 MHz — sound

Each sees the cartridge at a different base, so every analysis pass needs to
translate between "ROM file offset" and "address as some CPU sees it".
"""

import struct
from dataclasses import dataclass

# ---------------------------------------------------------------------------
# SH-2 address space
#
# The SH-2 encodes cache behaviour in the top three address bits, so the same
# physical location is visible at several addresses. Masking to 29 bits gives
# the underlying location for everything except the on-chip peripheral block.
# ---------------------------------------------------------------------------
SH2_CACHE_MASK = 0x1FFFFFFF

SH2_BOOT_ROM = (0x00000000, 0x00004000)   # per-CPU boot ROM, mirrored
SH2_SYSREG   = (0x00004000, 0x00004100)   # 32X system registers
SH2_VDPREG   = (0x00004100, 0x00004200)   # 32X VDP registers
SH2_CRAM     = (0x00004200, 0x00004400)   # 512-byte palette
SH2_ROM      = (0x02000000, 0x02400000)   # cartridge, 4 MB window
SH2_FB       = (0x04000000, 0x04020000)   # 128 KB frame buffer
SH2_FB_OW    = (0x04020000, 0x04040000)   # frame buffer overwrite image
SH2_SDRAM    = (0x06000000, 0x06040000)   # 256 KB work RAM
SH2_ONCHIP   = (0xFFFFF000, 0x100000000)  # DMAC / SCI / WDT / FRT / INTC

SDRAM_BASE = SH2_SDRAM[0]
SDRAM_SIZE = SH2_SDRAM[1] - SH2_SDRAM[0]
ROM_BASE = SH2_ROM[0]

# ---------------------------------------------------------------------------
# 68000 address space (with the 32X adapter enabled)
# ---------------------------------------------------------------------------
M68K_FB      = (0x840000, 0x860000)   # 32X frame buffer window
M68K_FB_OW   = (0x860000, 0x880000)
M68K_ROM_LO  = (0x880000, 0x900000)   # first 512 KB of cart, fixed
M68K_ROM_BANK= (0x900000, 0xA00000)   # 1 MB banked cart window
M68K_Z80     = (0xA00000, 0xA10000)
M68K_IO      = (0xA10000, 0xA10020)   # controller / version ports
M68K_MARS    = (0xA15100, 0xA15400)   # 32X system + PWM registers
M68K_VDP     = (0xC00000, 0xC00020)   # Genesis VDP
M68K_RAM     = (0xFF0000, 0x1000000)  # 64 KB work RAM


# The SH-2 splits its address space into areas by the top bits: 0x00000000 is
# cached and 0x20000000 is cache-through, and those two are mirrors of the same
# memory. The higher areas are *not* mirrors — 0x60000000 is the cache address
# array and 0xC0000000 the cache data array, both real distinct storage. This
# game copies a routine into the cache data array and executes it there, so
# collapsing those would alias real code onto the boot ROM.
SH2_CACHE_ARRAY = (0xC0000000, 0xC0001000)   # cache data array, 4 KB


def sh2_phys(addr: int) -> int:
    """Collapse the cached/cache-through mirrors to a canonical address."""
    if addr < 0x40000000:
        return addr & SH2_CACHE_MASK
    return addr


def sh2_region(addr: int) -> str:
    a = sh2_phys(addr)
    for name, (lo, hi) in (
        ("bootrom", SH2_BOOT_ROM), ("sysreg", SH2_SYSREG), ("vdpreg", SH2_VDPREG),
        ("cram", SH2_CRAM), ("rom", SH2_ROM), ("fb", SH2_FB),
        ("fb_ow", SH2_FB_OW), ("sdram", SH2_SDRAM), ("onchip", SH2_ONCHIP),
    ):
        if lo <= a < hi:
            return name
    return "unmapped"


@dataclass
class MarsHeader:
    """The 32X-specific header at cartridge offset 0x3C0."""
    module_name: str
    version: int
    sh2_src: int      # ROM offset of the SH-2 image
    sh2_dst: int      # SDRAM destination
    sh2_size: int
    master_start: int
    slave_start: int
    master_vbr: int
    slave_vbr: int


class Rom:
    """A 32X cartridge image with address translation for each CPU."""

    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()

    # -- header ------------------------------------------------------------
    def u32(self, off: int) -> int:
        return struct.unpack_from(">I", self.data, off)[0]

    def u16(self, off: int) -> int:
        return struct.unpack_from(">H", self.data, off)[0]

    def string(self, off: int, n: int) -> str:
        return self.data[off:off + n].decode("ascii", "replace").rstrip("\x00 ")

    @property
    def title(self) -> str:
        return self.string(0x150, 48)

    @property
    def serial(self) -> str:
        return self.string(0x180, 14)

    @property
    def region(self) -> str:
        return self.string(0x1F0, 3)

    def mars_header(self) -> MarsHeader:
        return MarsHeader(
            module_name=self.string(0x3C0, 16),
            version=self.u32(0x3D0),
            sh2_src=self.u32(0x3D4),
            sh2_dst=self.u32(0x3D8),
            sh2_size=self.u32(0x3DC),
            master_start=self.u32(0x3E0),
            slave_start=self.u32(0x3E4),
            master_vbr=self.u32(0x3E8),
            slave_vbr=self.u32(0x3EC),
        )

    def genesis_checksum(self):
        """(stored, computed) — Mega Drive header checksum over 0x200..end."""
        stored = self.u16(0x18E)
        ck = 0
        for i in range(0x200, len(self.data), 2):
            ck = (ck + struct.unpack_from(">H", self.data, i)[0]) & 0xFFFF
        return stored, ck

    # -- address translation ------------------------------------------------
    def sh2_to_off(self, addr: int):
        """SH-2 address -> ROM file offset, or None if it is not in cartridge."""
        a = sh2_phys(addr)
        if SH2_ROM[0] <= a < SH2_ROM[1]:
            off = a - SH2_ROM[0]
            return off if off < len(self.data) else None
        # The SDRAM image is a verbatim copy of a ROM block, so SDRAM addresses
        # inside that block are backed by known ROM bytes.
        h = self.mars_header()
        lo = SDRAM_BASE + h.sh2_dst
        if lo <= a < lo + h.sh2_size:
            return h.sh2_src + (a - lo)
        return None

    def off_to_sh2(self, off: int, prefer_sdram: bool = True) -> int:
        """ROM offset -> the SH-2 address the code actually runs at."""
        h = self.mars_header()
        if prefer_sdram and h.sh2_src <= off < h.sh2_src + h.sh2_size:
            return SDRAM_BASE + h.sh2_dst + (off - h.sh2_src)
        return ROM_BASE + off

    def m68k_to_off(self, addr: int, bank: int = 0):
        """68000 address -> ROM file offset, or None."""
        if M68K_ROM_LO[0] <= addr < M68K_ROM_LO[1]:
            return addr - M68K_ROM_LO[0]
        if M68K_ROM_BANK[0] <= addr < M68K_ROM_BANK[1]:
            return bank * 0x100000 + (addr - M68K_ROM_BANK[0])
        if addr < 0x400000:
            return addr if addr < len(self.data) else None
        return None

    def off_to_m68k(self, off: int) -> int:
        return M68K_ROM_LO[0] + off if off < 0x80000 else off

    def read(self, off: int, n: int) -> bytes:
        return self.data[off:off + n]

    def sdram_image(self) -> bytes:
        h = self.mars_header()
        return self.data[h.sh2_src:h.sh2_src + h.sh2_size]
