/* Machine state for the 32X side: memory, VDP registers, and the comm
 * registers through which the (not yet present) 68000 drives the SH-2s. */
#ifndef MARS_H
#define MARS_H

#include <stdint.h>
#include <setjmp.h>
#include "sh2.h"

#define MARS_ROM_MAX   (4u * 1024 * 1024)
#define MARS_SDRAM     (256u * 1024)
#define MARS_FB        (128u * 1024)
#define MARS_MAX_CMDS  64

typedef struct {
    uint8_t  rom[MARS_ROM_MAX];
    uint32_t rom_size;
    uint8_t  sdram[MARS_SDRAM];
    uint8_t  fb[MARS_FB];
    uint8_t  cache[4096];
    uint8_t  cram[512];
    uint8_t  onchip[512];    /* FRT / WDT / DMAC / SCI, as a plain store */

    /* 32X system registers */
    uint16_t adapter, intctl, bank;
    uint16_t comm[8];

    /* 32X VDP registers */
    uint16_t bitmap_mode, shift, fill_len, fill_start, fill_data, fbctl;

    uint32_t ticks;          /* stands in for elapsed time when polling */
    uint32_t unknown;        /* accesses outside the modelled map */
    uint32_t missing;        /* indirect transfers with no recompiled target */

    uint16_t cmds[MARS_MAX_CMDS];
    unsigned ncmd, cmd_at;
} Mars;

extern Mars mars;

/* The SH-2 polls hardware in loops that only a real machine would break out of.
 * When nothing is left to feed it, unwind rather than spin forever. */
extern jmp_buf mars_bail;
#define MARS_BAIL_IDLE  1
#define MARS_BAIL_BUDGET 2

void mars_set_commands(const uint16_t *cmds, unsigned n);

#endif
