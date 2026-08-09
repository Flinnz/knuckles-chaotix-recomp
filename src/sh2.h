/* SH-2 execution context and the semantics too subtle to inline at each site.
 *
 * The recompiler emits straight-line C for the easy instructions and calls into
 * this header for the ones where getting a flag wrong is both easy and fatal:
 * the divide step, the multiply-accumulates, and the carry/overflow forms.
 * Keeping them here means there is exactly one copy of each to review.
 */
#ifndef SH2_H
#define SH2_H

#include <stdint.h>
#include <string.h>

typedef struct SH2 {
    uint32_t r[16];
    uint32_t pr, gbr, vbr;
    uint32_t mach, macl;
    /* SR is kept unpacked: every instruction touches T, and div1 needs M and Q
     * on their own. `imask` holds the interrupt-mask bits so that stc/ldc sr
     * round-trips a value the game may have saved to memory. */
    uint32_t t, m, q, s, imask;
    uint32_t pc;
    int slave;              /* 0 = master SH-2, 1 = slave */
} SH2;

/* Memory. Supplied by the runtime; the generated code only ever goes through
 * these, so the address map lives in one place. */
uint8_t  sh2_r8(SH2 *c, uint32_t a);
uint16_t sh2_r16(SH2 *c, uint32_t a);
uint32_t sh2_r32(SH2 *c, uint32_t a);
void     sh2_w8(SH2 *c, uint32_t a, uint8_t v);
void     sh2_w16(SH2 *c, uint32_t a, uint16_t v);
void     sh2_w32(SH2 *c, uint32_t a, uint32_t v);

/* Transfer to an address that was not statically known. */
void     sh2_call_indirect(SH2 *c, uint32_t addr);
void     sh2_unimplemented(SH2 *c, uint32_t addr, const char *what);

/* --- status register packing ------------------------------------------- */
static inline uint32_t sh2_get_sr(const SH2 *c) {
    return (c->t & 1) | ((c->s & 1) << 1) | ((c->imask & 0xF) << 4)
         | ((c->q & 1) << 8) | ((c->m & 1) << 9);
}
static inline void sh2_set_sr(SH2 *c, uint32_t v) {
    c->t = v & 1; c->s = (v >> 1) & 1; c->imask = (v >> 4) & 0xF;
    c->q = (v >> 8) & 1; c->m = (v >> 9) & 1;
}

/* --- arithmetic with carry / overflow ----------------------------------- */
static inline void sh2_addc(SH2 *c, int n, int m) {
    uint32_t a = c->r[n], b = c->r[m], t = c->t;
    uint32_t s1 = a + b, s2 = s1 + t;
    c->t = (s1 < a) | (s2 < s1);
    c->r[n] = s2;
}
static inline void sh2_subc(SH2 *c, int n, int m) {
    uint32_t a = c->r[n], b = c->r[m], t = c->t;
    uint32_t d1 = a - b, d2 = d1 - t;
    c->t = (a < b) | (d1 < t);
    c->r[n] = d2;
}
static inline void sh2_addv(SH2 *c, int n, int m) {
    int32_t a = (int32_t)c->r[n], b = (int32_t)c->r[m];
    int32_t s = (int32_t)((uint32_t)a + (uint32_t)b);
    c->t = ((a < 0) == (b < 0)) && ((s < 0) != (a < 0));
    c->r[n] = (uint32_t)s;
}
static inline void sh2_subv(SH2 *c, int n, int m) {
    int32_t a = (int32_t)c->r[n], b = (int32_t)c->r[m];
    int32_t d = (int32_t)((uint32_t)a - (uint32_t)b);
    c->t = ((a < 0) != (b < 0)) && ((d < 0) != (a < 0));
    c->r[n] = (uint32_t)d;
}
static inline void sh2_negc(SH2 *c, int n, int m) {
    uint32_t b = c->r[m], t = c->t;
    uint32_t d = 0u - b, r = d - t;
    c->t = (0u < b) | (d < t);
    c->r[n] = r;
}

/* --- the divide step ----------------------------------------------------
 * The SH-2 has no divide instruction: a 32-bit division is 32 of these, and
 * the quotient bit falls out of Q, M and T. The exact carry/borrow bookkeeping
 * below is the whole instruction, so it is transcribed rather than simplified.
 */
static inline void sh2_div0s(SH2 *c, int n, int m) {
    c->q = (c->r[n] >> 31) & 1;
    c->m = (c->r[m] >> 31) & 1;
    c->t = c->q ^ c->m;
}
static inline void sh2_div0u(SH2 *c) { c->q = c->m = c->t = 0; }

static inline void sh2_div1(SH2 *c, int n, int m) {
    uint32_t tmp0, tmp2 = c->r[m];
    uint32_t old_q = c->q, tmp1;
    c->q = (c->r[n] >> 31) & 1;
    c->r[n] = (c->r[n] << 1) | (c->t & 1);
    if (!old_q) {
        if (!c->m) {
            tmp0 = c->r[n];
            c->r[n] -= tmp2;
            tmp1 = c->r[n] > tmp0;
            c->q = c->q ? (tmp1 == 0) : tmp1;
        } else {
            tmp0 = c->r[n];
            c->r[n] += tmp2;
            tmp1 = c->r[n] < tmp0;
            c->q = c->q ? tmp1 : (tmp1 == 0);
        }
    } else {
        if (!c->m) {
            tmp0 = c->r[n];
            c->r[n] += tmp2;
            tmp1 = c->r[n] < tmp0;
            c->q = c->q ? (tmp1 == 0) : tmp1;
        } else {
            tmp0 = c->r[n];
            c->r[n] -= tmp2;
            tmp1 = c->r[n] > tmp0;
            c->q = c->q ? tmp1 : (tmp1 == 0);
        }
    }
    c->t = (c->q == c->m);
}

/* --- multiply / multiply-accumulate ------------------------------------- */
static inline void sh2_dmuls(SH2 *c, int n, int m) {
    int64_t p = (int64_t)(int32_t)c->r[n] * (int64_t)(int32_t)c->r[m];
    c->mach = (uint32_t)((uint64_t)p >> 32);
    c->macl = (uint32_t)p;
}
static inline void sh2_dmulu(SH2 *c, int n, int m) {
    uint64_t p = (uint64_t)c->r[n] * (uint64_t)c->r[m];
    c->mach = (uint32_t)(p >> 32);
    c->macl = (uint32_t)p;
}

/* mac.w: 16x16 accumulated into MACH:MACL. With the S bit set the result
 * saturates to 48 bits instead of wrapping. */
static inline void sh2_macw(SH2 *c, int n, int m) {
    int32_t a = (int16_t)sh2_r16(c, c->r[m]); c->r[m] += 2;
    int32_t b = (int16_t)sh2_r16(c, c->r[n]); c->r[n] += 2;
    int64_t acc = ((int64_t)(int32_t)c->mach << 32) | (uint32_t)c->macl;
    int64_t prod = (int64_t)a * (int64_t)b;
    if (c->s) {
        int64_t sum = acc + prod;
        const int64_t hi = 0x00007FFFFFFFFFFFLL, lo = -0x0000800000000000LL;
        if (sum > hi) sum = hi;
        if (sum < lo) sum = lo;
        acc = sum;
    } else {
        acc += prod;
    }
    c->mach = (uint32_t)((uint64_t)acc >> 32);
    c->macl = (uint32_t)acc;
}

static inline void sh2_macl(SH2 *c, int n, int m) {
    int64_t a = (int32_t)sh2_r32(c, c->r[m]); c->r[m] += 4;
    int64_t b = (int32_t)sh2_r32(c, c->r[n]); c->r[n] += 4;
    int64_t acc = ((int64_t)(int32_t)c->mach << 32) | (uint32_t)c->macl;
    acc += a * b;
    c->mach = (uint32_t)((uint64_t)acc >> 32);
    c->macl = (uint32_t)acc;
}

/* --- misc --------------------------------------------------------------- */
static inline void sh2_tas(SH2 *c, uint32_t a) {
    uint8_t v = sh2_r8(c, a);
    c->t = (v == 0);
    sh2_w8(c, a, (uint8_t)(v | 0x80));
}
static inline uint32_t sh2_swapb(uint32_t v) {
    return (v & 0xFFFF0000u) | ((v & 0xFF) << 8) | ((v >> 8) & 0xFF);
}
static inline uint32_t sh2_swapw(uint32_t v) {
    return (v << 16) | (v >> 16);
}
static inline uint32_t sh2_xtrct(uint32_t m, uint32_t n) {
    return (m << 16) | (n >> 16);
}
static inline int sh2_cmpstr(uint32_t a, uint32_t b) {
    uint32_t x = a ^ b;
    return ((x & 0xFF000000u) == 0) || ((x & 0x00FF0000u) == 0)
        || ((x & 0x0000FF00u) == 0) || ((x & 0x000000FFu) == 0);
}

#endif /* SH2_H */
