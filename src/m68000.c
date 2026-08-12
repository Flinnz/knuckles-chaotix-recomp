/* The trampoline recompiled 68000 code returns to.
 *
 * Every control transfer hands back an address instead of calling, so this loop
 * is the only thing on the C stack no matter how deep the 68000's own call
 * chain goes — and a `pea`/`rts` pair, or a return address the engine computes,
 * needs nothing special because a return is just an address like any other.
 *
 * It stops when the address has no recompiled block. That is not a failure
 * mode to be papered over: it is how an indirect transfer whose target
 * discovery never resolved reports itself, and the address it stops on names
 * the thing to go and find.
 */
#include "m68000.h"

/* Discovery works in cartridge offsets, because the 68000 sees the same bytes
 * through more than one window and an offset is the only stable coordinate —
 * so the generated code names offsets, and a PC that arrives from the
 * interpreter names an address. Below 0x400000 the two are the same number,
 * which is why the boot ran either way and hid this until the engine proper
 * was reached through the 0x880000 window. */
static uint32_t canon68k(uint32_t a) {
    /* The first 256 bytes are the adapter's, not the cartridge's: the vectors
     * plus the BIOS helper the game calls at 0x0000C0, which src/gen68k.c
     * assembles. Recompiled code holds the cartridge image of that region — a
     * table of pointers — so it must never run there. Naming an offset the
     * table cannot hold sends it to the interpreter, which reads what is
     * actually there. */
    if (a < 0x100u) return 0xFFFFFFFFu;
    if (a >= 0x880000u && a < 0x900000u) return a - 0x880000u;
    /* And 0x900000-0x9FFFFF is *not* an offset at all. It is the banked window,
     * and which megabyte of the cartridge it shows is a register the game
     * writes at 0xA15104 — so the same address is different code at different
     * moments, and no static translation can name it. This folded it to bank 0
     * unconditionally, which was invisible for as long as the game never called
     * through the window: the engine's `jsr 0x928EEC` at 0x88F688 wants bank 2,
     * where the bytes are a dispatch prologue, and ran bank 0's data instead,
     * decoding as `ori.b #110,(a0)+` and falling into the halt stub at
     * 0x880B2E. Handing the whole window to the interpreter is what is
     * available: it reads through src/gen68k.c, which honours the register. */
    if (a >= 0x900000u && a < 0xA00000u) return 0xFFFFFFFFu;
    return a;
}

static int lookup(uint32_t addr) {
    unsigned lo = 0, hi = m68k_function_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (m68k_functions[mid].addr == addr) return (int)mid;
        if (m68k_functions[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return -1;
}

int32_t m68k_fuel;
uint32_t m68k_insns;

/* `budget` is cycles, and it is spent inside the blocks rather than counted
 * here — see M68K_BLOCK in m68000.h. It was transfers once, which could not
 * bound a poll loop at all, because an intra-function loop is a `goto` in the
 * generated C and never comes back here; then instructions, which bounded it
 * but had to be converted from cycles by a constant that is only right in one
 * of the engine's two phases. */
/* Can recompiled code be entered here? The interpreter needs to know, so that a
 * hand-over can stop at the end of the gap rather than at the end of a slice. */
int m68k_dispatchable(uint32_t addr) {
    return lookup(canon68k(addr)) >= 0;
}

uint32_t m68k_run(M68K *c, uint32_t addr, unsigned budget, int *known) {
    m68k_fuel = budget ? (int32_t)budget : INT32_MAX;
    for (;;) {
        uint32_t off = canon68k(addr);
        int i = lookup(off);
        if (i < 0) {
            if (known) *known = 0;
            return addr;
        }
        addr = m68k_functions[i].fn(c, off);
        if (addr == M68K_YIELD) {
            if (known) *known = 1;
            return c->pc;
        }
    }
}

void m68k_reset_recomp(M68K *c, uint32_t *pc) {
    for (unsigned i = 0; i < 8; i++) c->d[i] = c->a[i] = 0;
    c->usp = 0;
    c->n = c->z = c->v = c->c = c->x = 0;
    c->s = 1;
    c->t = 0;
    c->imask = 7;
    c->a[7] = M68K_R32(0);
    *pc = M68K_R32(4);
}

int m68k_interrupt(M68K *c, uint32_t *pc, unsigned level) {
    if (level != 7 && level <= c->imask) return 0;
    uint16_t sr = m68k_get_sr(c);
    m68k_set_sr(c, (uint16_t)(sr | 0x2000));      /* supervisor: swaps SP */
    c->a[7] -= 4; M68K_W32(c->a[7], *pc);
    c->a[7] -= 2; M68K_W16(c->a[7], sr);
    c->imask = (uint8_t)level;
    c->t = 0;
    *pc = M68K_R32((24u + level) * 4u);
    return 1;
}
