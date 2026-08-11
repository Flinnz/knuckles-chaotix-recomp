/* The context and helpers that recompiled 68000 code runs against.
 *
 * Two things differ from the SH-2 side, and both make this simpler rather than
 * harder.
 *
 * **There is a real stack.** `bsr`/`jsr` push a return address and `rts` pops
 * it, so nothing has to be modelled around a link register. The flat trampoline
 * is kept anyway, for the same reason it exists there: every control transfer
 * returns its destination and `m68k_run` loops, so no C frame is created and
 * code that computes a return address — `pea` then `rts`, which this engine
 * does — works by construction.
 *
 * **The condition codes are explicit.** Five bytes rather than a lazy record of
 * the last operation. The 68000 sets them in enough different ways that a lazy
 * scheme needs a tag per rule, and the rules are exactly what a semantics test
 * has to pin; explicit is what can be checked one instruction at a time. X is
 * genuinely separate from C — `addx`/`subx`/`roxl`/`roxr` read it and most
 * instructions leave it alone — so it is its own field.
 *
 * Memory goes through the same `m68k_read_memory_*` the interpreter uses, so
 * recompiled code and Musashi share one address space and one can be swapped
 * for the other.
 */
#ifndef M68000_H
#define M68000_H

#include <stdint.h>

typedef struct {
    uint32_t d[8];
    uint32_t a[8];           /* a[7] is whichever stack pointer is active */
    uint32_t usp;            /* the other one, parked */
    uint8_t  n, z, v, c, x;  /* condition codes */
    uint8_t  s, t;           /* supervisor, trace */
    uint8_t  imask;          /* interrupt priority mask */
} M68K;

unsigned int m68k_read_memory_8(unsigned int a);
unsigned int m68k_read_memory_16(unsigned int a);
unsigned int m68k_read_memory_32(unsigned int a);
void m68k_write_memory_8(unsigned int a, unsigned int v);
void m68k_write_memory_16(unsigned int a, unsigned int v);
void m68k_write_memory_32(unsigned int a, unsigned int v);

#define M68K_R8(a)     ((uint32_t)m68k_read_memory_8((a)))
#define M68K_R16(a)    ((uint32_t)m68k_read_memory_16((a)))
#define M68K_R32(a)    ((uint32_t)m68k_read_memory_32((a)))
#define M68K_W8(a, v)  m68k_write_memory_8((a), (v))
#define M68K_W16(a, v) m68k_write_memory_16((a), (v))
#define M68K_W32(a, v) m68k_write_memory_32((a), (v))

/* --- status register ------------------------------------------------------ */
static inline uint16_t m68k_get_sr(const M68K *c) {
    return (uint16_t)((c->t ? 0x8000 : 0) | (c->s ? 0x2000 : 0)
                      | ((c->imask & 7) << 8)
                      | (c->x ? 0x10 : 0) | (c->n ? 0x08 : 0) | (c->z ? 0x04 : 0)
                      | (c->v ? 0x02 : 0) | (c->c ? 0x01 : 0));
}

/* Changing S swaps the stack pointers, which is the whole reason USP exists. */
static inline void m68k_set_sr(M68K *c, uint16_t v) {
    uint8_t s = (v & 0x2000) != 0;
    if (s != c->s) {
        uint32_t other = c->usp;
        c->usp = c->a[7];
        c->a[7] = other;
        c->s = s;
    }
    c->t = (v & 0x8000) != 0;
    c->imask = (v >> 8) & 7;
    c->x = (v & 0x10) != 0; c->n = (v & 0x08) != 0; c->z = (v & 0x04) != 0;
    c->v = (v & 0x02) != 0; c->c = (v & 0x01) != 0;
}

static inline void m68k_set_ccr(M68K *c, uint8_t v) {
    c->x = (v & 0x10) != 0; c->n = (v & 0x08) != 0; c->z = (v & 0x04) != 0;
    c->v = (v & 0x02) != 0; c->c = (v & 0x01) != 0;
}

/* --- condition codes ------------------------------------------------------
 * `sz` is a literal at every call site, so the masking folds away.
 */
#define M68K_MASK(sz) ((sz) == 4 ? 0xFFFFFFFFu : (1u << ((sz) * 8)) - 1u)
#define M68K_MSB(sz)  (1u << ((sz) * 8 - 1))

static inline void m68k_f_logic(M68K *c, uint32_t r, int sz) {
    r &= M68K_MASK(sz);
    c->n = (r & M68K_MSB(sz)) != 0;
    c->z = r == 0;
    c->v = 0; c->c = 0;
}

/* r = d + s */
static inline void m68k_f_add(M68K *c, uint32_t s, uint32_t d, uint32_t r, int sz) {
    uint32_t m = M68K_MASK(sz), b = M68K_MSB(sz);
    s &= m; d &= m; r &= m;
    c->n = (r & b) != 0;
    c->z = r == 0;
    c->v = (((s ^ r) & (d ^ r)) & b) != 0;
    c->c = c->x = (((s & d) | (~r & (s | d))) & b) != 0;
}

/* r = d - s. `cmp` uses the same rules but must not touch X, hence the split. */
static inline void m68k_f_cmp(M68K *c, uint32_t s, uint32_t d, uint32_t r, int sz) {
    uint32_t m = M68K_MASK(sz), b = M68K_MSB(sz);
    s &= m; d &= m; r &= m;
    c->n = (r & b) != 0;
    c->z = r == 0;
    c->v = (((s ^ d) & (r ^ d)) & b) != 0;
    c->c = (((s & ~d) | (r & (s | ~d))) & b) != 0;
}

static inline void m68k_f_sub(M68K *c, uint32_t s, uint32_t d, uint32_t r, int sz) {
    m68k_f_cmp(c, s, d, r, sz);
    c->x = c->c;
}

/* addx/subx clear Z only when the result is non-zero — a multi-precision add
 * has to see "zero so far" survive across the words. */
static inline void m68k_f_addx(M68K *c, uint32_t s, uint32_t d, uint32_t r, int sz) {
    uint8_t z = c->z;
    m68k_f_add(c, s, d, r, sz);
    c->z = z && c->z;
}

static inline void m68k_f_subx(M68K *c, uint32_t s, uint32_t d, uint32_t r, int sz) {
    uint8_t z = c->z;
    m68k_f_sub(c, s, d, r, sz);
    c->z = z && c->z;
}

/* --- the trampoline -------------------------------------------------------
 * One row per basic block, so a transfer to any known address finds the
 * function that owns it and enters it there. Generated by
 * tools/recompile68k.py.
 */
typedef struct { uint32_t addr; uint32_t (*fn)(M68K *, uint32_t); } M68KEntry;
extern const M68KEntry m68k_functions[];
extern const unsigned m68k_function_count;

/* Run from `addr` for at most `budget` transfers and return where it stopped.
 * `*known` comes back 0 when it stopped because there is no recompiled block
 * there, which is how an unrecovered indirect target reports itself, and 1 when
 * the budget simply ran out. A budget of 0 means "until it stops".
 */
uint32_t m68k_run(M68K *c, uint32_t addr, unsigned budget, int *known);

/* Reset from the vector table, and take an autovectored interrupt.
 *
 * The exception frame is the 68000's: the interrupt switches to supervisor
 * mode, pushes the return address and then the old status register, masks to
 * its own level, and vectors through 24 + level. Nothing here reads that frame
 * except `rte`, which pops it back. */
void m68k_reset_recomp(M68K *c, uint32_t *pc);
int m68k_interrupt(M68K *c, uint32_t *pc, unsigned level);

#endif
