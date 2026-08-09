/* 32X memory map and VDP, as seen by the SH-2 side.
 *
 * This replaces the flat array the recompiler tests ran against. Only the SH-2
 * view is modelled: the 68000 half of the machine is not here yet, so the few
 * places where the two CPUs rendezvous (the comm registers) are driven by the
 * host instead — see mars_set_command().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mars.h"

Mars mars;
jmp_buf mars_bail;
static uint32_t idle_reads, budget;

/* ---------------------------------------------------------------- helpers */
static inline uint32_t canon(uint32_t a) {
    /* Only the cached and cache-through areas are mirrors of each other; the
     * cache arrays higher up are distinct storage. */
    return a < 0x40000000u ? (a & 0x1FFFFFFFu) : a;
}

/* Every SH-2 memory access ticks this. Hooking only the VDP status register
 * was not enough: the slave's dispatch loop polls a comm byte and never touches
 * the VDP, so it spun forever in native code with nothing to unwind it. */
void mars_tick_budget(void) {
    if (++budget > 20000000u) longjmp(mars_bail, MARS_BAIL_BUDGET);
}

void mars_reset_budget(void) { budget = 0; idle_reads = 0; }

static void trap(const char *what, uint32_t a) {
    if (mars.unknown++ < 16)
        fprintf(stderr, "  [mem] %s 0x%08X\n", what, a);
}

/* The SH-2 spends much of the boot polling hardware. Nothing here advances on
 * its own, so a free-running counter stands in for the passage of time and
 * lets those loops terminate. */
static uint16_t vdp_status(void) {
    uint32_t t = mars.ticks++;
    uint16_t s = 0;
    if ((t & 0x3F) < 0x08) s |= 0x8000;      /* VBLK */
    if ((t & 0x07) < 0x02) s |= 0x4000;      /* HBLK */
    s |= mars.fbctl & 0x0003;                /* FEN / FS as written */
    s |= 0x2000;                             /* PEN: palette accessible */
    return s;
}

static void autofill(void) {
    uint32_t addr = mars.fill_start;
    for (uint32_t i = 0; i <= mars.fill_len; i++) {
        uint32_t o = (addr & 0x1FFFFu);
        mars.fb[o] = (uint8_t)(mars.fill_data >> 8);
        mars.fb[o + 1] = (uint8_t)mars.fill_data;
        /* The fill wraps within a 512-byte line, which is what makes it
         * useful for clearing one scanline at a time. */
        addr = (addr & 0xFF00u) | ((addr + 2) & 0xFFu);
    }
}

/* ------------------------------------------------------------- dispatch ---
 * Returns 1 if the access was handled as a register, 0 if it is plain memory.
 */
int mars_reg_write_sh2(uint32_t a, uint32_t v, int size) {
    if (a >= 0x4000 && a < 0x4100) {          /* system registers */
        switch (a & 0xFE) {
        case 0x00: mars.adapter = (uint16_t)v; return 1;
        case 0x02: mars.intctl = (uint16_t)v; return 1;
        case 0x04: mars.bank = (uint16_t)v; return 1;
        case 0x14: case 0x16: case 0x18: case 0x1A:
        case 0x1C: case 0x1E: return 1;       /* interrupt clears */
        case 0x20:                            /* comm 0: the 68000 rendezvous */
            mars.comm[0] = (uint16_t)v;
            if (v == 0 && mars.cmd_at < mars.ncmd) {
                /* The handler acknowledged; hand it the next queued command. */
                mars.comm[0] = mars.cmds[mars.cmd_at++];
                if (mars.cmd_at >= mars.ncmd) mars.cmd_at = mars.ncmd = 0;
            }
            return 1;
        default:
            if ((a & 0xFE) >= 0x20 && (a & 0xFE) < 0x30) {
                mars.comm[((a & 0xFE) - 0x20) / 2] = (uint16_t)v;
                return 1;
            }
            return 1;
        }
    }
    if (a >= 0x4100 && a < 0x4200) {          /* VDP registers */
        switch (a & 0xFE) {
        case 0x00: mars.bitmap_mode = (uint16_t)v; return 1;
        case 0x02: mars.shift = (uint16_t)v; return 1;
        case 0x04: mars.fill_len = (uint16_t)v; return 1;
        case 0x06: mars.fill_start = (uint16_t)v; return 1;
        case 0x08: mars.fill_data = (uint16_t)v; autofill(); return 1;
        case 0x0A: mars.fbctl = (uint16_t)v; return 1;
        default: return 1;
        }
    }
    if (a >= 0x4200 && a < 0x4400) {          /* palette */
        uint32_t o = a - 0x4200;
        if (size == 1) mars.cram[o] = (uint8_t)v;
        else { mars.cram[o] = (uint8_t)(v >> 8); mars.cram[o + 1] = (uint8_t)v; }
        return 1;
    }
    return 0;
}

int mars_reg_read_sh2(uint32_t a, uint32_t *out) {
    if (a >= 0x4000 && a < 0x4100) {
        switch (a & 0xFE) {
        case 0x00: *out = mars.adapter; return 1;
        case 0x02: *out = mars.intctl; return 1;
        case 0x04: *out = mars.bank; return 1;
        default:
            if ((a & 0xFE) >= 0x20 && (a & 0xFE) < 0x30) {
                *out = mars.comm[((a & 0xFE) - 0x20) / 2];
                if ((a & 0xFE) == 0x20) {
                    /* Waiting on a command the 68000 will never send. */
                    if (*out == 0 && ++idle_reads > 200000)
                        longjmp(mars_bail, MARS_BAIL_IDLE);
                    if (*out) idle_reads = 0;
                }
                return 1;
            }
            *out = 0; return 1;
        }
    }
    if (a >= 0x4100 && a < 0x4200) {
        switch (a & 0xFE) {
        case 0x00: *out = mars.bitmap_mode; return 1;
        case 0x02: *out = mars.shift; return 1;
        case 0x0A: *out = vdp_status(); return 1;
        default: *out = 0; return 1;
        }
    }
    if (a >= 0x4200 && a < 0x4400) {
        uint32_t o = a - 0x4200;
        *out = ((uint32_t)mars.cram[o] << 8) | mars.cram[o + 1];
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- memory --- */
#define IN(a, lo, hi) ((a) >= (lo) && (a) < (hi))

static uint8_t *resolve(uint32_t a, uint32_t need) {
    if (IN(a, 0x06000000u, 0x06040000u)) return &mars.sdram[a - 0x06000000u];
    if (IN(a, 0x04000000u, 0x04020000u)) return &mars.fb[a - 0x04000000u];
    if (IN(a, 0x04020000u, 0x04040000u)) return &mars.fb[a - 0x04020000u];
    if (IN(a, 0x02000000u, 0x02000000u + MARS_ROM_MAX)) {
        uint32_t o = a - 0x02000000u;
        return o + need <= mars.rom_size ? &mars.rom[o] : NULL;
    }
    if (IN(a, 0xC0000000u, 0xC0001000u)) return &mars.cache[a - 0xC0000000u];
    /* SH-2 on-chip peripherals: the free-running timer, watchdog, DMAC and
     * serial block. The boot code programs the FRT and never reads anything
     * back that it did not write, so backing them with plain storage is
     * enough to get through init. */
    if (a >= 0xFFFFFE00u) return &mars.onchip[a - 0xFFFFFE00u];
    return NULL;
}

/* The overwrite image drops zero bytes instead of storing them, which is how
 * sprites are drawn without a mask. */
static int is_overwrite(uint32_t a) {
    return IN(a, 0x04020000u, 0x04040000u);
}

uint8_t sh2_r8(SH2 *c, uint32_t a) {
    mars_tick_budget();
    (void)c; a = canon(a);
    uint32_t v;
    if (a < 0x10000u && mars_reg_read_sh2(a, &v)) return (uint8_t)((a & 1) ? v : v >> 8);
    uint8_t *p = resolve(a, 1);
    if (p) return *p;
    trap("r8", a);
    return 0;
}
uint16_t sh2_r16(SH2 *c, uint32_t a) {
    mars_tick_budget();
    (void)c; a = canon(a);
    uint32_t v;
    if (a < 0x10000u && mars_reg_read_sh2(a, &v)) return (uint16_t)v;
    uint8_t *p = resolve(a, 2);
    if (p) return (uint16_t)((p[0] << 8) | p[1]);
    trap("r16", a);
    return 0;
}
uint32_t sh2_r32(SH2 *c, uint32_t a) {
    mars_tick_budget();
    (void)c; a = canon(a);
    if (a < 0x10000u) {
        uint32_t hi, lo;
        if (mars_reg_read_sh2(a, &hi) && mars_reg_read_sh2(a + 2, &lo)) return (hi << 16) | lo;
    }
    uint8_t *p = resolve(a, 4);
    if (p) return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                | ((uint32_t)p[2] << 8) | p[3];
    trap("r32", a);
    return 0;
}
void sh2_w8(SH2 *c, uint32_t a, uint8_t v) {
    mars_tick_budget();
    (void)c; uint32_t ra = canon(a);
    if (ra < 0x10000u && mars_reg_write_sh2(ra, v, 1)) return;
    if (is_overwrite(ra) && v == 0) return;
    uint8_t *p = resolve(ra, 1);
    if (p) { if (ra < 0x02000000u || ra >= 0x03000000u) *p = v; return; }
    trap("w8", ra);
}
void sh2_w16(SH2 *c, uint32_t a, uint16_t v) {
    mars_tick_budget();
    (void)c; uint32_t ra = canon(a);
    if (ra < 0x10000u && mars_reg_write_sh2(ra, v, 2)) return;
    uint8_t *p = resolve(ra, 2);
    if (p) {
        if (is_overwrite(ra)) {
            if (v >> 8) p[0] = (uint8_t)(v >> 8);
            if (v & 0xFF) p[1] = (uint8_t)v;
        } else { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
        return;
    }
    trap("w16", ra);
}
void sh2_w32(SH2 *c, uint32_t a, uint32_t v) {
    sh2_w16(c, a, (uint16_t)(v >> 16));
    sh2_w16(c, a + 2, (uint16_t)v);
}

/* ------------------------------------------------------------ transfers --- */
typedef struct { uint32_t addr; void (*fn)(SH2 *); } SH2Entry;
extern const SH2Entry sh2_functions[];
extern const unsigned sh2_function_count;

void sh2_call_indirect(SH2 *c, uint32_t addr) {
    /* `jmp` is translated as call-then-return, so a hardware dispatch loop -
     * which on real silicon never returns - becomes unbounded C recursion here
     * and overflows the native stack. Bound it until the codegen models tail
     * transfers properly. */
    static unsigned depth;
    if (depth > 2000) { mars.deep++; longjmp(mars_bail, MARS_BAIL_DEPTH); }
    addr = canon(addr);
    for (unsigned i = 0; i < sh2_function_count; i++)
        if (sh2_functions[i].addr == addr) {
            depth++; sh2_functions[i].fn(c); depth--; return;
        }
    if (mars.missing++ < 16)
        fprintf(stderr, "  [call] no recompiled function at 0x%08X\n", addr);
}

void sh2_unimplemented(SH2 *c, uint32_t addr, const char *what) {
    (void)c;
    fprintf(stderr, "  [sh2] unimplemented at 0x%08X: %s\n", addr, what);
}

void mars_post_command(uint16_t cmd) {
    if (mars.ncmd < MARS_MAX_CMDS) mars.cmds[mars.ncmd++] = cmd;
}

void mars_set_commands(const uint16_t *cmds, unsigned n) {
    if (n > MARS_MAX_CMDS) n = MARS_MAX_CMDS;
    memcpy(mars.cmds, cmds, n * sizeof *cmds);
    mars.ncmd = n;
    mars.cmd_at = 0;
}
