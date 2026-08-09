/* Flat memory and an entry point for running recompiled SH-2 code natively.
 *
 * This is the test rig, not the game runtime: one big SDRAM-shaped array, no
 * hardware registers. It exists so recompiled code can be executed and its
 * results checked against answers computed independently of any SH-2 model.
 */
#include <stdio.h>
#include <stdlib.h>
#include "sh2.h"

#define BASE 0x06000000u
#define SIZE (4u * 1024 * 1024)

static uint8_t mem[SIZE];

static inline uint32_t off(uint32_t a) {
    /* Collapse the cached / cache-through mirrors, as the hardware does. */
    if (a < 0x40000000u) a &= 0x1FFFFFFFu;
    return a - BASE;
}

uint8_t sh2_r8(SH2 *c, uint32_t a) {
    (void)c; uint32_t o = off(a);
    return o < SIZE ? mem[o] : 0;
}
uint16_t sh2_r16(SH2 *c, uint32_t a) {
    (void)c; uint32_t o = off(a);
    return o + 1 < SIZE ? (uint16_t)((mem[o] << 8) | mem[o + 1]) : 0;
}
uint32_t sh2_r32(SH2 *c, uint32_t a) {
    (void)c; uint32_t o = off(a);
    if (o + 3 >= SIZE) return 0;
    return ((uint32_t)mem[o] << 24) | ((uint32_t)mem[o + 1] << 16)
         | ((uint32_t)mem[o + 2] << 8) | mem[o + 3];
}
void sh2_w8(SH2 *c, uint32_t a, uint8_t v) {
    (void)c; uint32_t o = off(a); if (o < SIZE) mem[o] = v;
}
void sh2_w16(SH2 *c, uint32_t a, uint16_t v) {
    (void)c; uint32_t o = off(a);
    if (o + 1 < SIZE) { mem[o] = (uint8_t)(v >> 8); mem[o + 1] = (uint8_t)v; }
}
void sh2_w32(SH2 *c, uint32_t a, uint32_t v) {
    (void)c; uint32_t o = off(a);
    if (o + 3 < SIZE) {
        mem[o] = (uint8_t)(v >> 24); mem[o + 1] = (uint8_t)(v >> 16);
        mem[o + 2] = (uint8_t)(v >> 8); mem[o + 3] = (uint8_t)v;
    }
}

typedef struct { uint32_t addr; void (*fn)(SH2 *); } SH2Entry;
extern const SH2Entry sh2_functions[];
extern const unsigned sh2_function_count;

void sh2_call_indirect(SH2 *c, uint32_t addr) {
    if (addr < 0x40000000u) addr &= 0x1FFFFFFFu;
    for (unsigned i = 0; i < sh2_function_count; i++) {
        if (sh2_functions[i].addr == addr) { sh2_functions[i].fn(c); return; }
    }
    fprintf(stderr, "no recompiled function at 0x%08X\n", addr);
    exit(2);
}

void sh2_unimplemented(SH2 *c, uint32_t addr, const char *what) {
    (void)c;
    fprintf(stderr, "unimplemented at 0x%08X: %s\n", addr, what);
    exit(3);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s image.bin nresults\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fread(mem, 1, SIZE, f);
    fclose(f);

    SH2 c;
    memset(&c, 0, sizeof c);
    c.r[15] = 0x06040000u;          /* stack at the top of SDRAM */
    sh2_call_indirect(&c, BASE);

    int n = atoi(argv[2]);
    for (int i = 0; i < n; i++)
        printf("%u\n", sh2_r32(&c, 0x06008000u + (uint32_t)i * 4));
    return 0;
}
