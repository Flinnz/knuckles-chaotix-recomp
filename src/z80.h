/* The Mega Drive's sound sub-CPU.
 *
 * A plain interpreter, because the Z80 here is 4 KB of driver that the 68000
 * writes into RAM at run time — there is nothing to recompile statically, and
 * the reference logs every instruction it executes with the full register
 * state, so an interpreter can be held to that line for line.
 *
 * The core knows nothing about the machine: memory and I/O go through the four
 * hooks below, which src/genz80.c supplies.
 */
#ifndef Z80_H
#define Z80_H

#include <stdint.h>

typedef struct {
    /* The main set. `f` is a real flag byte including the two undocumented
     * bits, because the reference prints AF and they would otherwise differ on
     * nearly every line. */
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t a2, f2, b2, c2, d2, e2, h2, l2;      /* the alternate set */
    uint16_t ix, iy, sp, pc;
    uint8_t i, r;                /* r's low seven bits count refreshes */
    uint8_t iff1, iff2, im;
    uint8_t halted;
    uint8_t irq;                 /* the line, as the VDP holds it */
    uint8_t ei_hold;             /* EI defers acceptance by one instruction */
    uint16_t wz;                 /* MEMPTR: `bit n,(hl)` shows its high byte */

    int32_t fuel;                /* cycles left in the slice */
    unsigned long insns;
} Z80;

/* The bus, supplied by src/genz80.c. */
uint8_t z80_rd(uint16_t a);
void    z80_wr(uint16_t a, uint8_t v);
uint8_t z80_io_rd(uint16_t port);
void    z80_io_wr(uint16_t port, uint8_t v);

/* One line per instruction, in the reference tracer's field format, when
 * `z80_tracing` is set — see src/genz80.c. */
extern int z80_tracing;
void z80_trace_line(const Z80 *c);

void     z80_reset(Z80 *c);
/* Run about that many cycles. Returns what it actually spent, which overshoots
 * by up to one instruction — the caller carries the difference, the way every
 * other CPU here does. */
unsigned z80_run(Z80 *c, int cycles);

#endif
