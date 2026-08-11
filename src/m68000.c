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

static int lookup(uint32_t addr) {
    unsigned lo = 0, hi = m68k_function_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (m68k_functions[mid].addr == addr) return (int)mid;
        if (m68k_functions[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return -1;
}

uint32_t m68k_run(M68K *c, uint32_t addr) {
    for (;;) {
        int i = lookup(addr);
        if (i < 0) return addr;
        addr = m68k_functions[i].fn(c, addr);
    }
}
