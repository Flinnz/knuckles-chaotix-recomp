/* The Z80 as part of the machine: its RAM, the 68000's view of it, and the
 * arbitration that decides which of the two owns the bus. See src/genz80.c. */
#ifndef GENZ80_H
#define GENZ80_H

#include <stdint.h>
#include "z80.h"

extern Z80 z80;
extern uint8_t z80_ram[8 * 1024];
extern uint32_t z80_bytes_written, z80_resets, z80_bank_writes;

void     genz80_init(void);
int      genz80_runnable(void);
unsigned genz80_slice(int cycles);

/* The 68000's accesses into the Z80's half of the map — RAM, the sound chips,
 * the bank register, bus request and reset. Return 1 if the address was one of
 * theirs. */
int genz80_read(uint32_t a, uint8_t *out);
int genz80_write(uint32_t a, uint8_t v);
int genz80_read16(uint32_t a, uint16_t *out);
int genz80_write16(uint32_t a, uint16_t v);

/* One line per Z80 instruction, in the reference tracer's field format, for
 * tools/diffz80.py. */
int  z80_trace_open(const char *path, unsigned long max_lines);
void z80_trace_close(void);

#endif
