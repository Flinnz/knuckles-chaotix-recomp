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
    uint32_t pc;             /* where a slice that ran out of fuel resumes */
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

/* --- slicing --------------------------------------------------------------
 *
 * The same mechanism as the SH-2's, and needed for the same reason: an
 * intra-function loop is a `goto` in the generated C and never reaches the
 * trampoline, so the budget below cannot bound it. The engine's rendezvous with
 * the master at 0x881868 is exactly that shape — it waits for the SH-2 to post
 * 0xFFFF — and it only ever terminated while the SH-2 answered inside the
 * 68000's own register write.
 *
 * A block that finds the fuel spent hands its own address back, so the run
 * resumes there. M68K_YIELD is 1 because a 68000 instruction address is always
 * even, and 0 is a legitimate address here, unlike on the SH-2 side.
 */
#define M68K_YIELD 1u

/* The fuel is **cycles**, not instructions.
 *
 * It was instructions, and `cpu_run` converted a cycle budget into one with a
 * single constant — `recomp_cpi`, measured at 11. One constant cannot be right
 * twice. 11.00 cycles an instruction is this engine's steady-state mix exactly,
 * and through the boot it sits in a `tst.w ($a15120)` / `bne` poll that costs
 * 13, so the translated build ran that phase 18% fast: 11,622 instructions a
 * frame where Musashi and the reference both run 9,834. It was the whole of the
 * 68000 residue, and for a while it was mistaken for the adapter being free.
 *
 * So there is no conversion any more. Each block carries what its instructions
 * actually cost, summed at build time from Musashi's own table — see
 * tools/m68kcycles.c — and `cpu_run` hands the fuel a cycle budget directly,
 * the way it already hands Musashi one.
 */
extern int32_t m68k_fuel;

/* And the instructions separately, because the fuel no longer counts them and
 * `--rate68k` still asks. */
extern uint32_t m68k_insns;

/* Block-entry trace, for `tools/diff68k.py --blocks`.
 *
 * Under `--recomp` the interpreter runs almost nothing, so `src/trace68k.c`'s
 * Musashi hook sees almost nothing, and the translated build was held to
 * "same picture, same command count" and no more. This is the SH-2 side's
 * answer: the reference logs every instruction, so filtering its stream to the
 * addresses that start a block yields exactly this sequence, and between two
 * block entries both machines ran the same straight-line code.
 *
 * `a` is a cartridge offset, which is the only stable coordinate — the same
 * bytes are reachable through more than one window — so the diff folds the
 * reference's addresses the same way before comparing.
 */
extern int m68k_trace;
void m68k_block(const M68K *c, uint32_t addr);

/* The yield is checked before the trace for the same reason it is on the SH-2
 * side, and it matters far more here: a sub-slice is about 2.7 instructions of
 * this engine's mix, so nearly every block was ending one slice and starting
 * the next, and logging before the fuel check recorded it once for the entry
 * that ran nothing and once for the entry that ran. The pair is identical, so
 * the alignment never noticed — but every trip count came out twice its true
 * length.
 */
/* `n` is the block's instructions and `cyc` what they cost. The branch it ends
 * on is charged on its own edge instead — Musashi's table holds the *taken*
 * cost of a `bcc` and adjusts on the other side, and the generated code emits
 * both sides, so it can do the same. */
#define M68K_BLOCK(c, a, n, cyc) do {                           \
    if (m68k_fuel <= 0) { (c)->pc = (a); return M68K_YIELD; }   \
    if (m68k_trace) m68k_block((c), (a));                       \
    m68k_fuel -= (cyc);                                         \
    m68k_insns += (n);                                          \
} while (0)

/* Run from `addr` for at most `budget` cycles and return where it stopped.
 * `*known` comes back 0 when it stopped because there is no recompiled block
 * there, which is how an unrecovered indirect target reports itself, and 1 when
 * the budget simply ran out. A budget of 0 means "until it stops".
 */
uint32_t m68k_run(M68K *c, uint32_t addr, unsigned budget, int *known);

/* Whether `m68k_run` could enter at this address — i.e. whether it is the start
 * of a recompiled block. What the interpreter uses to know when a gap is over. */
int m68k_dispatchable(uint32_t addr);

/* Reset from the vector table, and take an autovectored interrupt.
 *
 * The exception frame is the 68000's: the interrupt switches to supervisor
 * mode, pushes the return address and then the old status register, masks to
 * its own level, and vectors through 24 + level. Nothing here reads that frame
 * except `rte`, which pops it back. */
void m68k_reset_recomp(M68K *c, uint32_t *pc);
int m68k_interrupt(M68K *c, uint32_t *pc, unsigned level);

/* What taking one costs, from Musashi's own exception table: every autovectored
 * level is 44 on a 68000. Charged by the caller, which is the only place that
 * holds a cycle account between hand-overs. */
#define M68K_IRQ_CYCLES 44

#endif
