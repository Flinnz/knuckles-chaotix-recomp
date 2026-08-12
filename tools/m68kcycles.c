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
 * 65,536 bytes, one per opcode word, into the file named on the command line.
 *
 * A named file rather than stdout because stdout is a text stream on Windows:
 * a shell redirect there turns every 0x0A in the table into 0x0D 0x0A, and the
 * table is full of them — 10 cycles is the cost of a great many instructions.
 * That arrives as a longer-than-65,536-byte file, which `read_cycles` in
 * tools/recompile68k.py rejects, so it fails loudly rather than mispricing the
 * build; asking for the file by name is what keeps the bytes the bytes.
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

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <out.bin>\n", argv[0]);
        return 2;
    }
    m68k_init();
    /* The table is filled by m68k_init(); selecting the type is what points
     * CYC_INSTRUCTION at row 0, and asserts here that row 0 is the 68000. */
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);

    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror(argv[1]); return 1; }
    size_t n = fwrite(m68ki_cycles[0], 1, 0x10000, f);
    if (fclose(f) != 0 || n != 0x10000) {
        /* %lu rather than %zu: MinGW against the old msvcrt does not know %zu,
         * and 65,536 fits an unsigned long on every host here. */
        fprintf(stderr, "%s: wrote %lu of 65536 bytes\n",
                argv[1], (unsigned long)n);
        return 1;
    }
    return 0;
}
