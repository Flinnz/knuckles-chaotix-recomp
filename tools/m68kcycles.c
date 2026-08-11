/* Dump Musashi's 68000 cycle table, so the recompiler can charge what the
 * interpreter charges.
 *
 * The recompiled 68000 used to convert its instruction budget with one constant
 * — `recomp_cpi`, 11 — and one constant cannot be right twice: 11.00 cycles an
 * instruction is the engine's steady-state mix exactly, and through the boot it
 * sits in a 13-cycle poll of a 32X register and runs 18% fast. The fix is to
 * stop converting and sum each block's real cost at build time, which needs a
 * cost per opcode.
 *
 * Taking it from Musashi rather than from a second reading of the manual is the
 * same argument `src/m68k_testmem.c` makes for the semantics: the interpreter is
 * the oracle, it is measured right against the reference in both phases —
 * `tools/refpoll.py` prices the reference's own loops at 12.97 and 10.98 where
 * Musashi charges 13 and 11 — and a table copied from it agrees by construction
 * instead of by proofreading.
 *
 * `m68ki_cycles[0]` is the 68000's row; m68kmake fills it from the cycle columns
 * of `m68k_in.c`, expanded per addressing mode, so effective-address time is
 * already in each entry. What is *not* in it is everything the handlers add at
 * run time — a branch not taken, a shift's per-bit cost, `movem`'s per-register
 * cost — and `tools/recomp/m68kc.py` adds those itself, statically where the
 * count is an immediate and on the edge where it is a branch.
 *
 * 65,536 bytes on stdout, one per opcode word.
 */
#include <stdio.h>
#include "m68k.h"

extern unsigned char m68ki_cycles[][0x10000];

/* src/m68kconf.h wires the interrupt-acknowledge callback to the runtime's.
 * Nothing here runs an instruction, let alone takes an interrupt; this exists to
 * satisfy the link, as it does in src/m68k_testmem.c. */
int gen68k_int_ack(int level) { (void)level; return M68K_INT_ACK_AUTOVECTOR; }

/* Likewise: the table is built by m68k_init() without reading a byte of memory,
 * but the opcode handlers reference these. */
unsigned int m68k_read_memory_8(unsigned int a) { (void)a; return 0; }
unsigned int m68k_read_memory_16(unsigned int a) { (void)a; return 0; }
unsigned int m68k_read_memory_32(unsigned int a) { (void)a; return 0; }
void m68k_write_memory_8(unsigned int a, unsigned int v) { (void)a; (void)v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { (void)a; (void)v; }
void m68k_write_memory_32(unsigned int a, unsigned int v) { (void)a; (void)v; }

int main(void) {
    m68k_init();
    /* The table is filled by m68k_init(); selecting the type is what points
     * CYC_INSTRUCTION at row 0, and asserts here that row 0 is the 68000. */
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    return fwrite(m68ki_cycles[0], 1, 0x10000, stdout) == 0x10000 ? 0 : 1;
}
