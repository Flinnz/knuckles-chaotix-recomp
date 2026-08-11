/* Harness for the 68000 recompiler's semantics tests.
 *
 * Runs one assembled test program twice over identical memory — once through
 * Musashi, once through the recompiled C — and prints both result arrays for
 * tools/test_recomp68k.py to compare.
 *
 * Musashi is the oracle here rather than a table of expected values, and that
 * is the point: the numbers a case produces are easy to write down, but the
 * condition codes are exactly the part a manual gets wrong, and an independent
 * implementation of the same instruction set disagrees where a misreading
 * would. It is the same argument the reference traces make for the runtime,
 * applied one instruction at a time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68000.h"
#include "m68k.h"

#define MEM_SIZE 0x20000
static uint8_t mem[MEM_SIZE];

/* src/m68kconf.h wires the interrupt-acknowledge callback to the runtime's,
 * which is where the VDP clears its pending vertical interrupt. Nothing here
 * has a VDP or raises an interrupt, so this exists to satisfy the link. */
int gen68k_int_ack(int level) {
    (void)level;
    m68k_set_irq(0);
    return M68K_INT_ACK_AUTOVECTOR;
}

unsigned int m68k_read_memory_8(unsigned int a) {
    return a < MEM_SIZE ? mem[a] : 0;
}
unsigned int m68k_read_memory_16(unsigned int a) {
    a &= ~1u;
    return a + 1 < MEM_SIZE ? ((unsigned)mem[a] << 8) | mem[a + 1] : 0;
}
unsigned int m68k_read_memory_32(unsigned int a) {
    return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2);
}
void m68k_write_memory_8(unsigned int a, unsigned int v) {
    if (a < MEM_SIZE) mem[a] = (uint8_t)v;
}
void m68k_write_memory_16(unsigned int a, unsigned int v) {
    a &= ~1u;
    if (a + 1 < MEM_SIZE) { mem[a] = (uint8_t)(v >> 8); mem[a + 1] = (uint8_t)v; }
}
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v & 0xFFFF);
}

static void load(const char *path, uint32_t base) {
    memset(mem, 0, sizeof mem);
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fread(&mem[base], 1, MEM_SIZE - base, f);
    fclose(f);
    /* Reset vector: stack at the top of memory, entry at the link address. */
    m68k_write_memory_32(0, MEM_SIZE - 4);
    m68k_write_memory_32(4, base);
}

static void dump(const char *tag, uint32_t results, unsigned n) {
    printf("%s", tag);
    for (unsigned i = 0; i < n; i++) {
        uint32_t v = m68k_read_memory_32(results + i * 6);
        uint32_t f = m68k_read_memory_16(results + i * 6 + 4);
        printf(" %08X:%02X", v, f & 0x1F);
    }
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s BIN BASE RESULTS COUNT\n", argv[0]);
        return 1;
    }
    const char *binf = argv[1];
    uint32_t base = (uint32_t)strtoul(argv[2], NULL, 0);
    uint32_t results = (uint32_t)strtoul(argv[3], NULL, 0);
    unsigned count = (unsigned)strtoul(argv[4], NULL, 0);

    load(binf, base);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    /* `stop` ends the program, and Musashi will not run past it however many
     * cycles it is given. */
    for (int i = 0; i < 64 && !m68k_get_reg(NULL, M68K_REG_SR); i++)
        m68k_execute(100000);
    m68k_execute(2000000);
    dump("musashi", results, count);

    load(binf, base);
    M68K c;
    memset(&c, 0, sizeof c);
    c.a[7] = m68k_read_memory_32(0);
    c.s = 1;
    c.imask = 7;
    uint32_t stopped = m68k_run(&c, base, 0, NULL);
    dump("recomp ", results, count);
    fprintf(stderr, "  recompiled run left off at 0x%06X\n", stopped);
    return 0;
}
