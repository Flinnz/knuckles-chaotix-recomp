/* The Mega Drive half of the machine: the 68000's address space.
 *
 * The 68000 is the engine in this game — it decides what to draw and posts
 * commands to the SH-2s. The two CPUs meet only at the 32X communication
 * registers, which appear here at 0xA15120 and on the SH-2 side at 0x4020, so
 * both views index the same `mars.comm[]`.
 *
 * The Genesis VDP is modelled far enough to keep the game moving: the control
 * port's address/code latch, the data port, DMA from 68000 memory, and a status
 * register whose blanking bits advance so that polling loops terminate. Nothing
 * here renders a Mega Drive plane yet — the picture we are after comes from the
 * 32X framebuffer.
 */
#include <stdio.h>
#include <string.h>
#include "mars.h"
#include "m68k.h"

Gen gen;

#define IN(a, lo, hi) ((a) >= (lo) && (a) < (hi))

static uint32_t rom_at(uint32_t a) {
    if (IN(a, 0x880000u, 0x900000u)) return a - 0x880000u;
    if (IN(a, 0x900000u, 0xA00000u))
        return ((uint32_t)(mars.bank & 3) << 20) + (a - 0x900000u);
    if (a < 0x400000u) return a;
    return 0xFFFFFFFFu;
}

/* ------------------------------------------------------------ Genesis VDP */
static void vdp_dma(void) {
    uint32_t len = ((uint32_t)gen.vdpreg[20] << 8) | gen.vdpreg[19];
    uint32_t src = (((uint32_t)gen.vdpreg[23] & 0x7F) << 17)
                 | ((uint32_t)gen.vdpreg[22] << 9)
                 | ((uint32_t)gen.vdpreg[21] << 1);
    if (len == 0) len = 0x10000;
    if (gen.vdpreg[23] & 0x80) return;      /* fill / copy: not needed yet */
    for (uint32_t i = 0; i < len; i++) {
        uint16_t v = (uint16_t)m68k_read_memory_16(src);
        uint32_t a = gen.vdp_addr & 0xFFFFu;
        if ((gen.vdp_code & 7) == 1) {      /* to VRAM */
            gen.vram[a & 0xFFFEu] = (uint8_t)(v >> 8);
            gen.vram[(a & 0xFFFEu) + 1] = (uint8_t)v;
        } else if ((gen.vdp_code & 7) == 3) {                 /* to CRAM */
            gen.cram[(a >> 1) & 0x3F] = v;
        }
        src += 2;
        gen.vdp_addr += gen.vdpreg[15];
    }
    gen.dma_done++;
}

static void vdp_ctrl(uint16_t v) {
    if (gen.vdp_pending) {
        gen.vdp_addr = (gen.vdp_addr & 0x3FFFu) | ((uint32_t)(v & 3) << 14);
        gen.vdp_code = (uint8_t)((gen.vdp_code & 3) | ((v >> 2) & 0x3C));
        gen.vdp_pending = 0;
        if (gen.vdp_code & 0x20) vdp_dma();
        return;
    }
    if ((v & 0xC000) == 0x8000) {           /* register write */
        gen.vdpreg[(v >> 8) & 0x1F] = (uint8_t)v;
        return;
    }
    gen.vdp_addr = (gen.vdp_addr & 0xC000u) | (v & 0x3FFFu);
    gen.vdp_code = (uint8_t)((gen.vdp_code & 0x3C) | ((v >> 14) & 3));
    gen.vdp_pending = 1;
}

static void vdp_data(uint16_t v) {
    uint32_t a = gen.vdp_addr;
    switch (gen.vdp_code & 7) {
    case 1: gen.vram[a & 0xFFFEu] = (uint8_t)(v >> 8);
            gen.vram[(a & 0xFFFEu) + 1] = (uint8_t)v; break;
    case 3: gen.cram[(a >> 1) & 0x3F] = v; break;
    case 5: gen.vsram[(a >> 1) & 0x27] = v; break;
    default: break;
    }
    gen.vdp_addr += gen.vdpreg[15];
    gen.vdp_pending = 0;
}

static uint16_t vdp_status(void) {
    /* Bit 3 VBLANK, bit 2 HBLANK, bit 9 FIFO empty. Advancing these is what
     * lets the game's wait-for-vblank loops finish. */
    uint32_t t = gen.ticks++;
    uint16_t s = 0x0200;
    if ((t % 262) >= 224) s |= 0x0008;
    if ((t % 20) >= 16) s |= 0x0004;
    return s;
}

/* ------------------------------------------------------- 32X on the bus --- */
static int mars_reg_read(uint32_t a, uint16_t *out) {
    switch (a & ~1u) {
    case 0xA15100: *out = (uint16_t)(mars.adapter | 0x0080); return 1;  /* ADEN */
    case 0xA15102: *out = mars.intctl; return 1;
    case 0xA15104: *out = mars.bank; return 1;
    case 0xA1510A: *out = gen.dreq_ctl; return 1;
    default:
        if (IN(a & ~1u, 0xA15120u, 0xA15130u)) {
            *out = mars.comm[((a & ~1u) - 0xA15120u) / 2];
            return 1;
        }
        if (IN(a & ~1u, 0xA15130u, 0xA15140u)) { *out = 0; return 1; } /* PWM */
        /* The 32X VDP and palette, as the 68000 sees them. */
        if (IN(a & ~1u, 0xA15180u, 0xA15190u)) {
            uint32_t v; mars_reg_read_sh2(0x4100u + ((a & ~1u) - 0xA15180u), &v);
            *out = (uint16_t)v; return 1;
        }
        if (IN(a & ~1u, 0xA15200u, 0xA15400u)) {
            uint32_t v; mars_reg_read_sh2(0x4200u + ((a & ~1u) - 0xA15200u), &v);
            *out = (uint16_t)v; return 1;
        }
        if (IN(a & ~1u, 0xA15100u, 0xA15120u)) { *out = 0; return 1; }
        return 0;
    }
}

static int mars_reg_write(uint32_t a, uint16_t v) {
    switch (a & ~1u) {
    case 0xA15100: mars.adapter = v; return 1;
    case 0xA15102: mars.intctl = v; return 1;
    case 0xA15104: mars.bank = v; return 1;
    case 0xA1510A: gen.dreq_ctl = v; return 1;
    default:
        if (IN(a & ~1u, 0xA15120u, 0xA15130u)) {
            unsigned i = (unsigned)(((a & ~1u) - 0xA15120u) / 2);
            if (i == 0 && v) {
                /* Hand it to the SH-2 through the queue rather than writing
                 * comm[0] directly: the SH-2 clears that register to
                 * acknowledge, and would otherwise wipe the command. */
                mars_post_command(v);
                gen.cmd_posted++;
            } else {
                mars.comm[i] = v;
            }
            return 1;
        }
        if (IN(a & ~1u, 0xA15130u, 0xA15140u)) return 1;
        if (IN(a & ~1u, 0xA15180u, 0xA15190u))
            return mars_reg_write_sh2(0x4100u + ((a & ~1u) - 0xA15180u), v, 2);
        if (IN(a & ~1u, 0xA15200u, 0xA15400u))
            return mars_reg_write_sh2(0x4200u + ((a & ~1u) - 0xA15200u), v, 2);
        if (IN(a & ~1u, 0xA15100u, 0xA15120u)) return 1;   /* DREQ block */
        return 0;
    }
}

/* ------------------------------------------------------------ callbacks --- */
unsigned int m68k_read_memory_8(unsigned int a) {
    a &= 0xFFFFFFu;
    uint32_t o = rom_at(a);
    if (o != 0xFFFFFFFFu && o < mars.rom_size) return mars.rom[o];
    if (IN(a, 0xFF0000u, 0x1000000u)) return gen.ram[a & 0xFFFFu];
    return (unsigned)(m68k_read_memory_16(a & ~1u) >> ((a & 1) ? 0 : 8)) & 0xFF;
}

unsigned int m68k_read_memory_16(unsigned int a) {
    a &= 0xFFFFFEu;
    uint32_t o = rom_at(a);
    if (o != 0xFFFFFFFFu && o + 1 < mars.rom_size)
        return ((unsigned)mars.rom[o] << 8) | mars.rom[o + 1];
    if (IN(a, 0xFF0000u, 0x1000000u))
        return ((unsigned)gen.ram[a & 0xFFFFu] << 8) | gen.ram[(a & 0xFFFFu) + 1];
    if (IN(a, 0xC00000u, 0xC00010u)) {
        if ((a & 0x1F) < 4) return 0;              /* data port read */
        if ((a & 0x1F) < 8) return vdp_status();
        return (unsigned)((gen.ticks * 3) & 0xFFFF);   /* HV counter */
    }
    /* The 32X identifies itself here; the boot code refuses to run without it. */
    if (a == 0xA130EC) return 0x4D41;      /* "MA" */
    if (a == 0xA130EE) return 0x5253;      /* "RS" */
    if (IN(a, 0xA10000u, 0xA10020u)) {
        /* Version register: domestic, NTSC, no expansion, with the 32X bit. */
        if (a == 0xA10000) return 0x00A0;
        return gen.io[(a & 0x1F) >> 1];
    }
    uint16_t v;
    if (mars_reg_read(a, &v)) return v;
    if (gen.unknown_r++ < 12) fprintf(stderr, "  [68k] read 0x%06X\n", a);
    return 0;
}

unsigned int m68k_read_memory_32(unsigned int a) {
    return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2);
}

void m68k_write_memory_8(unsigned int a, unsigned int v) {
    a &= 0xFFFFFFu;
    if (IN(a, 0xFF0000u, 0x1000000u)) { gen.ram[a & 0xFFFFu] = (uint8_t)v; return; }
    if (IN(a, 0xA10000u, 0xA10020u)) { gen.io[(a & 0x1F) >> 1] = (uint16_t)v; return; }
    if (IN(a, 0xA00000u, 0xA10000u)) return;            /* Z80 space */
    if (IN(a, 0xA11000u, 0xA11400u)) return;            /* Z80 bus / reset */
    if (a == 0xA130F1) return;                          /* SRAM control */
    uint16_t cur = (uint16_t)m68k_read_memory_16(a & ~1u);
    uint16_t nv = (a & 1) ? (uint16_t)((cur & 0xFF00) | (v & 0xFF))
                          : (uint16_t)((cur & 0x00FF) | ((v & 0xFF) << 8));
    m68k_write_memory_16(a & ~1u, nv);
}

void m68k_write_memory_16(unsigned int a, unsigned int v) {
    a &= 0xFFFFFEu;
    uint16_t w = (uint16_t)v;
    if (IN(a, 0xFF0000u, 0x1000000u)) {
        gen.ram[a & 0xFFFFu] = (uint8_t)(w >> 8);
        gen.ram[(a & 0xFFFFu) + 1] = (uint8_t)w;
        return;
    }
    if (IN(a, 0xC00000u, 0xC00010u)) {
        if ((a & 0x1F) < 4) vdp_data(w);
        else if ((a & 0x1F) < 8) vdp_ctrl(w);
        return;
    }
    if (IN(a, 0xA10000u, 0xA10020u)) { gen.io[(a & 0x1F) >> 1] = w; return; }
    if (IN(a, 0xA00000u, 0xA10000u)) return;
    if (IN(a, 0xA11000u, 0xA11400u)) return;            /* Z80 bus / reset */
    if (mars_reg_write(a, w)) return;
    if (IN(a, 0x840000u, 0x880000u)) {                  /* 32X framebuffer */
        uint32_t o = (a - 0x840000u) & 0x1FFFEu;
        int over = a >= 0x860000u;
        if (!over || (w >> 8)) mars.fb[o] = (uint8_t)(w >> 8);
        if (!over || (w & 0xFF)) mars.fb[o + 1] = (uint8_t)w;
        return;
    }
    if (rom_at(a) != 0xFFFFFFFFu) return;               /* writes to ROM ignored */
    if (gen.unknown_w++ < 12) fprintf(stderr, "  [68k] write 0x%06X = %04X\n", a, w);
}

void m68k_write_memory_32(unsigned int a, unsigned int v) {
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v & 0xFFFF);
}
