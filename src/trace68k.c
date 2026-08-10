/* One line per 68000 instruction, for mechanical comparison against a
 * reference emulator's trace.
 *
 * The line format is the reference tracer's field format — PC, opcode word,
 * then the register state *before* the instruction executes — so the two
 * streams can be read side by side. The mnemonic is deliberately absent: the
 * diff tool annotates addresses using the project's own validated decoder
 * rather than a second disassembler here.
 *
 * Every instruction is written, with nothing collapsed. The reference tracer
 * does collapse repeats, but it reports each run as an exact "[Omitted: N]"
 * count, and a complete stream on our side plus those counts is what lets the
 * diff tool predict where each reference line should land — which in turn is
 * what makes "this loop ran a different number of times" a finding rather than
 * a lost alignment.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mars.h"
#include "m68k.h"

static FILE *tf;
static unsigned long emitted, limit;

static void hook(unsigned int pc) {
    if (limit && emitted >= limit) return;
    emitted++;

    uint32_t r[16];
    for (int i = 0; i < 16; i++)
        r[i] = (uint32_t)m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
    uint32_t sr = (uint32_t)m68k_get_reg(NULL, M68K_REG_SR);
    /* The reference prints the active stack pointer as a7 and the inactive one
     * as sp, which is the other of USP/ISP depending on the S bit. */
    uint32_t osp = (uint32_t)m68k_get_reg(NULL, (sr & 0x2000) ? M68K_REG_USP
                                                             : M68K_REG_ISP);
    char f[9];
    f[0] = (sr & 0x8000) ? 'T' : 't';
    f[1] = (sr & 0x2000) ? 'S' : 's';
    f[2] = (char)('0' + ((sr >> 8) & 7));
    f[3] = (sr & 0x01) ? 'C' : 'c';
    f[4] = (sr & 0x02) ? 'V' : 'v';
    f[5] = (sr & 0x04) ? 'Z' : 'z';
    f[6] = (sr & 0x08) ? 'N' : 'n';
    f[7] = (sr & 0x10) ? 'X' : 'x';
    f[8] = 0;

    fprintf(tf, "CPU  %06x  %04x  ", pc & 0xFFFFFFu,
            (unsigned)m68k_read_memory_16(pc));
    for (int i = 0; i < 8; i++) fprintf(tf, "d%d:%08x ", i, r[i]);
    for (int i = 0; i < 8; i++) fprintf(tf, "a%d:%08x ", i, r[8 + i]);
    fprintf(tf, "sp:%08x %s\n", osp, f);
}

/* Called once, before the 68000 runs. */
int trace68k_open(const char *path, unsigned long max_lines) {
    tf = fopen(path, "w");
    if (!tf) { perror(path); return 0; }
    limit = max_lines;
    m68k_set_instr_hook_callback(hook);
    return 1;
}

void trace68k_close(void) {
    if (!tf) return;
    fclose(tf);
    tf = NULL;
    m68k_set_instr_hook_callback(NULL);
    printf("  68000 trace: %lu instructions written%s\n", emitted,
           limit && emitted >= limit ? " (line limit reached)" : "");
}
