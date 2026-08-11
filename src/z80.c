/* A Z80 interpreter, decoded the way the instruction set is actually laid out.
 *
 * The opcode byte is three fields — x = op >> 6, y = (op >> 3) & 7, z = op & 7
 * — and every unprefixed instruction is one cell of that grid. Writing it that
 * way rather than as 256 cases is what makes complete coverage checkable by
 * reading: there is one line per cell, and a missing cell is a missing line
 * rather than a gap in a list nobody can hold in their head.
 *
 * Two things here are not optional even though they look like detail.
 *
 * The undocumented flag bits, X and Y, are bits 3 and 5 of F and hold bits 3
 * and 5 of whatever the last operation produced. The reference prints AF, so
 * without them almost every logged line would differ and the trace comparison
 * would be worthless. `sz53[]` and `sz53p[]` are those bits precomputed.
 *
 * The DD and FD prefixes do not simply mean "IX" and "IY". They replace HL, and
 * they replace `(hl)` with `(ix+d)`, but *not both in the same instruction*:
 * where the instruction already names memory, H and L stay themselves. That is
 * the `xy` parameter threaded through below, and `mem` is where the rule lives.
 */
#include <string.h>
#include "z80.h"

#define SF 0x80u
#define ZF 0x40u
#define YF 0x20u
#define HF 0x10u
#define XF 0x08u
#define VF 0x04u
#define NF 0x02u
#define CF 0x01u

static uint8_t sz53[256], sz53p[256], parity[256];
static int tables_done;

static void tables(void) {
    for (int i = 0; i < 256; i++) {
        int p = 0;
        for (int b = 0; b < 8; b++) p ^= (i >> b) & 1;
        parity[i] = p ? 0 : VF;
        sz53[i] = (uint8_t)((i & (SF | YF | XF)) | (i ? 0 : ZF));
        sz53p[i] = (uint8_t)(sz53[i] | parity[i]);
    }
    tables_done = 1;
}

void z80_reset(Z80 *c) {
    if (!tables_done) tables();
    memset(c, 0, sizeof *c);
    /* A Z80 comes out of reset with everything clear but AF and SP, which the
     * reference shows as 0xFFFF apiece on the machine's very first line. */
    c->a = c->f = 0xFF;
    c->sp = 0xFFFF;
}

int z80_tracing;

/* ------------------------------------------------------------- fetch/stack --
 *
 * R counts M1 cycles — opcode fetches — and not the operand bytes that follow
 * them, so `ld hl,nn` advances it once and not three times. It is not
 * bookkeeping: the driver reads it at 0x0876 as its source of randomness, so a
 * refresh counter that counts the wrong thing shows up directly in what the
 * sound does. A prefix is its own M1, which is why a DD- or CB-prefixed
 * instruction advances R twice and a DDCB one, whose last two bytes are read as
 * operands, advances it twice as well.
 */
static uint8_t opfetch(Z80 *c) {
    c->r = (uint8_t)((c->r & 0x80) | ((c->r + 1) & 0x7F));
    return z80_rd(c->pc++);
}

static uint8_t fetch(Z80 *c) {
    return z80_rd(c->pc++);
}

static uint16_t fetch16(Z80 *c) {
    uint16_t v = fetch(c);
    return (uint16_t)(v | (fetch(c) << 8));
}

static uint16_t rd16(uint16_t a) {
    return (uint16_t)(z80_rd(a) | (z80_rd((uint16_t)(a + 1)) << 8));
}

static void wr16(uint16_t a, uint16_t v) {
    z80_wr(a, (uint8_t)v);
    z80_wr((uint16_t)(a + 1), (uint8_t)(v >> 8));
}

static void push(Z80 *c, uint16_t v) {
    z80_wr(--c->sp, (uint8_t)(v >> 8));
    z80_wr(--c->sp, (uint8_t)v);
}

static uint16_t pop(Z80 *c) {
    uint16_t v = z80_rd(c->sp++);
    return (uint16_t)(v | (z80_rd(c->sp++) << 8));
}

/* -------------------------------------------------------------- registers -- */
#define HL(c) ((uint16_t)(((c)->h << 8) | (c)->l))
#define BC(c) ((uint16_t)(((c)->b << 8) | (c)->c))
#define DE(c) ((uint16_t)(((c)->d << 8) | (c)->e))
#define AF(c) ((uint16_t)(((c)->a << 8) | (c)->f))

static void set_hl(Z80 *c, uint16_t v) { c->h = (uint8_t)(v >> 8); c->l = (uint8_t)v; }
static void set_bc(Z80 *c, uint16_t v) { c->b = (uint8_t)(v >> 8); c->c = (uint8_t)v; }
static void set_de(Z80 *c, uint16_t v) { c->d = (uint8_t)(v >> 8); c->e = (uint8_t)v; }

/* The index register a prefix selected, or HL. */
static uint16_t idx(const Z80 *c, int xy) {
    return xy == 1 ? c->ix : xy == 2 ? c->iy : HL(c);
}

static void set_idx(Z80 *c, int xy, uint16_t v) {
    if (xy == 1) c->ix = v;
    else if (xy == 2) c->iy = v;
    else set_hl(c, v);
}

/* rp[] as the encoding orders it: BC, DE, HL-or-index, SP. */
static uint16_t rp(const Z80 *c, int p, int xy) {
    switch (p) {
    case 0: return BC(c);
    case 1: return DE(c);
    case 2: return idx(c, xy);
    default: return c->sp;
    }
}

static void set_rp(Z80 *c, int p, int xy, uint16_t v) {
    switch (p) {
    case 0: set_bc(c, v); break;
    case 1: set_de(c, v); break;
    case 2: set_idx(c, xy, v); break;
    default: c->sp = v; break;
    }
}

/* rp2[], which has AF where rp[] has SP. */
static uint16_t rp2(const Z80 *c, int p, int xy) {
    return p == 3 ? AF(c) : rp(c, p, xy);
}

static void set_rp2(Z80 *c, int p, int xy, uint16_t v) {
    if (p == 3) { c->a = (uint8_t)(v >> 8); c->f = (uint8_t)v; }
    else set_rp(c, p, xy, v);
}

/* Where `(hl)` points once a prefix has had its say. Returns the address and
 * consumes the displacement byte, which is why it must be called exactly once
 * per instruction that needs it and before any immediate operand is fetched. */
static uint16_t mem(Z80 *c, int xy) {
    if (!xy) return HL(c);
    int8_t d = (int8_t)fetch(c);
    uint16_t a = (uint16_t)(idx(c, xy) + d);
    c->wz = a;
    return a;
}

/* r[]: B C D E H L (HL) A. `xy` promotes H and L to the index halves, but only
 * where the instruction does not already name memory — the caller says which by
 * passing xy = 0 for the register side of a `ld r,(ix+d)`. */
static uint8_t get_r(Z80 *c, int n, int xy, uint16_t ea) {
    switch (n) {
    case 0: return c->b;
    case 1: return c->c;
    case 2: return c->d;
    case 3: return c->e;
    case 4: return xy == 1 ? (uint8_t)(c->ix >> 8)
                 : xy == 2 ? (uint8_t)(c->iy >> 8) : c->h;
    case 5: return xy == 1 ? (uint8_t)c->ix
                 : xy == 2 ? (uint8_t)c->iy : c->l;
    case 6: return z80_rd(ea);
    default: return c->a;
    }
}

static void set_r(Z80 *c, int n, int xy, uint16_t ea, uint8_t v) {
    switch (n) {
    case 0: c->b = v; break;
    case 1: c->c = v; break;
    case 2: c->d = v; break;
    case 3: c->e = v; break;
    case 4:
        if (xy == 1) c->ix = (uint16_t)((c->ix & 0xFF) | (v << 8));
        else if (xy == 2) c->iy = (uint16_t)((c->iy & 0xFF) | (v << 8));
        else c->h = v;
        break;
    case 5:
        if (xy == 1) c->ix = (uint16_t)((c->ix & 0xFF00) | v);
        else if (xy == 2) c->iy = (uint16_t)((c->iy & 0xFF00) | v);
        else c->l = v;
        break;
    case 6: z80_wr(ea, v); break;
    default: c->a = v; break;
    }
}

/* ------------------------------------------------------------------- alu -- */
static void add_a(Z80 *c, uint8_t v, unsigned carry) {
    unsigned r = c->a + v + carry;
    c->f = (uint8_t)(sz53[r & 0xFF]
        | ((r >> 8) & CF)
        | (((c->a ^ v ^ r) & 0x10) ? HF : 0)
        | ((((c->a ^ ~v) & (c->a ^ r)) & 0x80) ? VF : 0));
    c->a = (uint8_t)r;
}

static void sub_a(Z80 *c, uint8_t v, unsigned carry) {
    unsigned r = c->a - v - carry;
    c->f = (uint8_t)(sz53[r & 0xFF] | NF
        | ((r >> 8) & CF)
        | (((c->a ^ v ^ r) & 0x10) ? HF : 0)
        | ((((c->a ^ v) & (c->a ^ r)) & 0x80) ? VF : 0));
    c->a = (uint8_t)r;
}

static void cp_a(Z80 *c, uint8_t v) {
    unsigned r = c->a - v;
    /* Compare takes X and Y from the *operand*, not the result — one of the
     * places the undocumented bits are not simply "bits of the answer". */
    c->f = (uint8_t)((sz53[r & 0xFF] & (SF | ZF)) | (v & (YF | XF)) | NF
        | ((r >> 8) & CF)
        | (((c->a ^ v ^ r) & 0x10) ? HF : 0)
        | ((((c->a ^ v) & (c->a ^ r)) & 0x80) ? VF : 0));
}

static void and_a(Z80 *c, uint8_t v) { c->a &= v; c->f = (uint8_t)(sz53p[c->a] | HF); }
static void xor_a(Z80 *c, uint8_t v) { c->a ^= v; c->f = sz53p[c->a]; }
static void or_a(Z80 *c, uint8_t v)  { c->a |= v; c->f = sz53p[c->a]; }

static void alu(Z80 *c, int op, uint8_t v) {
    switch (op) {
    case 0: add_a(c, v, 0); break;
    case 1: add_a(c, v, c->f & CF); break;
    case 2: sub_a(c, v, 0); break;
    case 3: sub_a(c, v, c->f & CF); break;
    case 4: and_a(c, v); break;
    case 5: xor_a(c, v); break;
    case 6: or_a(c, v); break;
    default: cp_a(c, v); break;
    }
}

static uint8_t inc8(Z80 *c, uint8_t v) {
    uint8_t r = (uint8_t)(v + 1);
    c->f = (uint8_t)((c->f & CF) | sz53[r]
        | ((r & 0x0F) ? 0 : HF) | (r == 0x80 ? VF : 0));
    return r;
}

static uint8_t dec8(Z80 *c, uint8_t v) {
    uint8_t r = (uint8_t)(v - 1);
    c->f = (uint8_t)((c->f & CF) | NF | sz53[r]
        | (((r & 0x0F) == 0x0F) ? HF : 0) | (r == 0x7F ? VF : 0));
    return r;
}

static void add16(Z80 *c, int xy, int p) {
    uint16_t a = idx(c, xy), b = rp(c, p, xy);
    unsigned r = a + b;
    c->wz = (uint16_t)(a + 1);
    c->f = (uint8_t)((c->f & (SF | ZF | VF))
        | (((a ^ b ^ r) & 0x1000) ? HF : 0)
        | ((r >> 16) & CF)
        | ((r >> 8) & (YF | XF)));
    set_idx(c, xy, (uint16_t)r);
}

static void adc16(Z80 *c, int p) {
    uint16_t a = HL(c), b = rp(c, p, 0);
    unsigned r = a + b + (c->f & CF);
    c->wz = (uint16_t)(a + 1);
    c->f = (uint8_t)(((r & 0xFFFF) ? 0 : ZF)
        | (((a ^ b ^ r) & 0x1000) ? HF : 0)
        | ((r >> 16) & CF)
        | (((~(a ^ b) & (a ^ r)) & 0x8000) ? VF : 0)
        | ((r >> 8) & (SF | YF | XF)));
    set_hl(c, (uint16_t)r);
}

static void sbc16(Z80 *c, int p) {
    uint16_t a = HL(c), b = rp(c, p, 0);
    unsigned r = a - b - (c->f & CF);
    c->wz = (uint16_t)(a + 1);
    c->f = (uint8_t)(NF | ((r & 0xFFFF) ? 0 : ZF)
        | (((a ^ b ^ r) & 0x1000) ? HF : 0)
        | ((r >> 16) & CF)
        | ((((a ^ b) & (a ^ r)) & 0x8000) ? VF : 0)
        | ((r >> 8) & (SF | YF | XF)));
    set_hl(c, (uint16_t)r);
}

/* The one instruction whose table nobody remembers: DAA's correction depends on
 * H, C and N together, and it is the only place N is read rather than written. */
static void daa(Z80 *c) {
    unsigned add = 0, carry = c->f & CF;
    if ((c->f & HF) || (c->a & 0x0F) > 9) add = 6;
    if (carry || c->a > 0x99) { add |= 0x60; carry = CF; }
    if (c->f & NF) {
        uint8_t before = c->a;
        c->a = (uint8_t)(c->a - add);
        c->f = (uint8_t)(NF | carry | sz53p[c->a]
            | (((before ^ c->a) & 0x10) ? HF : 0));
    } else {
        uint8_t before = c->a;
        c->a = (uint8_t)(c->a + add);
        c->f = (uint8_t)(carry | sz53p[c->a]
            | (((before ^ c->a) & 0x10) ? HF : 0));
    }
}

/* --------------------------------------------------------------- rotates -- */
static uint8_t rot(Z80 *c, int op, uint8_t v) {
    uint8_t r, carry;
    switch (op) {
    case 0: carry = (uint8_t)(v >> 7); r = (uint8_t)((v << 1) | carry); break; /* rlc */
    case 1: carry = (uint8_t)(v & 1); r = (uint8_t)((v >> 1) | (carry << 7)); break;
    case 2: carry = (uint8_t)(v >> 7); r = (uint8_t)((v << 1) | (c->f & CF)); break;
    case 3: carry = (uint8_t)(v & 1); r = (uint8_t)((v >> 1) | ((c->f & CF) << 7)); break;
    case 4: carry = (uint8_t)(v >> 7); r = (uint8_t)(v << 1); break;           /* sla */
    case 5: carry = (uint8_t)(v & 1); r = (uint8_t)((v >> 1) | (v & 0x80)); break;
    /* sll is undocumented and shifts a 1 in, which is the whole of what makes
     * it different from sla. */
    case 6: carry = (uint8_t)(v >> 7); r = (uint8_t)((v << 1) | 1); break;
    default: carry = (uint8_t)(v & 1); r = (uint8_t)(v >> 1); break;           /* srl */
    }
    c->f = (uint8_t)(sz53p[r] | carry);
    return r;
}

/* A on its own: the accumulator rotates keep S, Z and P/V. */
static void rot_a(Z80 *c, int op) {
    uint8_t v = c->a, carry;
    switch (op) {
    case 0: carry = (uint8_t)(v >> 7); c->a = (uint8_t)((v << 1) | carry); break;
    case 1: carry = (uint8_t)(v & 1); c->a = (uint8_t)((v >> 1) | (carry << 7)); break;
    case 2: carry = (uint8_t)(v >> 7); c->a = (uint8_t)((v << 1) | (c->f & CF)); break;
    default: carry = (uint8_t)(v & 1); c->a = (uint8_t)((v >> 1) | ((c->f & CF) << 7)); break;
    }
    c->f = (uint8_t)((c->f & (SF | ZF | VF)) | (c->a & (YF | XF)) | carry);
}

static void bit_n(Z80 *c, int n, uint8_t v, int from_mem, uint16_t ea) {
    uint8_t r = (uint8_t)(v & (1u << n));
    c->f = (uint8_t)((c->f & CF) | HF | (r ? (r & SF) : (ZF | VF)));
    /* X and Y come from the operand for a register, and from the high byte of
     * the address for `(ix+d)` — for `(hl)` they come from MEMPTR, which is the
     * only reason this core keeps one. */
    if (from_mem) c->f |= (uint8_t)((ea >> 8) & (YF | XF));
    else c->f |= (uint8_t)(v & (YF | XF));
}

/* ----------------------------------------------------------- block moves -- */
static void ldi_ldd(Z80 *c, int inc) {
    uint8_t v = z80_rd(HL(c));
    z80_wr(DE(c), v);
    set_hl(c, (uint16_t)(HL(c) + inc));
    set_de(c, (uint16_t)(DE(c) + inc));
    set_bc(c, (uint16_t)(BC(c) - 1));
    uint8_t n = (uint8_t)(v + c->a);
    c->f = (uint8_t)((c->f & (SF | ZF | CF))
        | ((n & 0x02) ? YF : 0) | ((n & 0x08) ? XF : 0)
        | (BC(c) ? VF : 0));
}

static void cpi_cpd(Z80 *c, int inc) {
    uint8_t v = z80_rd(HL(c));
    unsigned r = c->a - v;
    uint8_t half = (uint8_t)(((c->a ^ v ^ r) & 0x10) ? HF : 0);
    set_hl(c, (uint16_t)(HL(c) + inc));
    set_bc(c, (uint16_t)(BC(c) - 1));
    uint8_t n = (uint8_t)(r - (half ? 1 : 0));
    c->f = (uint8_t)((c->f & CF) | NF | half
        | (sz53[r & 0xFF] & SF) | ((r & 0xFF) ? 0 : ZF)
        | ((n & 0x02) ? YF : 0) | ((n & 0x08) ? XF : 0)
        | (BC(c) ? VF : 0));
}

/* ----------------------------------------------------------------- costs --
 *
 * The base T-states of every unprefixed opcode. A conditional that is taken,
 * an index prefix and a `(ix+d)` displacement each add their own on top, at the
 * point the instruction decides — see `step`.
 *
 * These are what the Z80's share of a hand-over is spent in, so an error here
 * shows up as the driver running at the wrong rate against the reference's
 * instruction count per frame rather than as a wrong answer.
 */
static const uint8_t cyc_main[256] = {
     4,10, 7, 6, 4, 4, 7, 4, 4,11, 7, 6, 4, 4, 7, 4,
     8,10, 7, 6, 4, 4, 7, 4,12,11, 7, 6, 4, 4, 7, 4,
     7,10,16, 6, 4, 4, 7, 4, 7,11,16, 6, 4, 4, 7, 4,
     7,10,13, 6,11,11,10, 4, 7,11,13, 6, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     7, 7, 7, 7, 7, 7, 4, 7, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
     5,10,10,10,10,11, 7,11, 5,10,10, 4,10,17, 7,11,
     5,10,10,11,10,11, 7,11, 5, 4,10,11,10, 4, 7,11,
     5,10,10,19,10,11, 7,11, 5, 4,10, 4,10, 4, 7,11,
     5,10,10, 4,10,11, 7,11, 5, 6,10, 4,10, 4, 7,11,
};

/* ------------------------------------------------------------------ step -- */
static void step(Z80 *c, int xy);

static void prefix_cb(Z80 *c, int xy) {
    /* With DD or FD the displacement comes *before* the opcode, and the result
     * is written back to the named register as well as to memory — the
     * undocumented form of every CB operation. */
    uint16_t ea = 0;
    uint8_t op;
    if (xy) {
        int8_t d = (int8_t)fetch(c);
        ea = (uint16_t)(idx(c, xy) + d);
        c->wz = ea;
        op = fetch(c);              /* an operand read, not an M1 */
    } else {
        op = opfetch(c);
        ea = HL(c);
    }
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    uint8_t v = xy ? z80_rd(ea) : get_r(c, z, 0, ea);

    if (x == 0) {
        v = rot(c, y, v);
    } else if (x == 1) {
        bit_n(c, y, v, z == 6 || xy, xy ? ea : c->wz);
        /* What is left after the prefix bytes, which `cyc_main` has already
         * charged: `bit n,r` is 8 all told, `bit n,(hl)` 12, `bit n,(ix+d)` 20
         * — and DD and CB have cost 4 apiece to get here. */
        c->fuel -= xy ? 12 : (z == 6 ? 8 : 4);
        return;
    } else if (x == 2) {
        v = (uint8_t)(v & ~(1u << y));
    } else {
        v = (uint8_t)(v | (1u << y));
    }
    if (xy) {
        z80_wr(ea, v);
        if (z != 6) set_r(c, z, 0, 0, v);
        c->fuel -= 15;                   /* 23 less the two prefixes */
    } else {
        c->fuel -= z == 6 ? 11 : 4;      /* 15 and 8 less the prefix */
        set_r(c, z, 0, ea, v);
    }
}

static void prefix_ed(Z80 *c) {
    uint8_t op = opfetch(c);
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    /* Every ED instruction is at least 8, of which `cyc_main` charged 4 for the
     * prefix byte; each case below adds what it costs over that. */
    c->fuel -= 4;
    if (x == 1) {
        switch (z) {
        case 0: {                                   /* in r[y],(c) */
            uint8_t v = z80_io_rd(BC(c));
            c->wz = (uint16_t)(BC(c) + 1);
            if (y != 6) set_r(c, y, 0, 0, v);
            c->f = (uint8_t)((c->f & CF) | sz53p[v]);
            c->fuel -= 4;
            return;
        }
        case 1:                                     /* out (c),r[y] */
            z80_io_wr(BC(c), y == 6 ? 0 : get_r(c, y, 0, 0));
            c->wz = (uint16_t)(BC(c) + 1);
            c->fuel -= 4;
            return;
        case 2:
            if (q) adc16(c, p); else sbc16(c, p);
            c->fuel -= 7;
            return;
        case 3: {
            uint16_t nn = fetch16(c);
            c->wz = (uint16_t)(nn + 1);
            if (q) set_rp(c, p, 0, rd16(nn)); else wr16(nn, rp(c, p, 0));
            c->fuel -= 12;
            return;
        }
        case 4: {                                   /* neg */
            uint8_t v = c->a;
            c->a = 0;
            sub_a(c, v, 0);
            return;
        }
        case 5:                                     /* retn / reti */
            c->iff1 = c->iff2;
            c->pc = pop(c);
            c->wz = c->pc;
            c->fuel -= 6;
            return;
        case 6:                                     /* im */
            c->im = (uint8_t)((y & 3) < 2 ? (y & 3 ? 0 : 0) : (y & 3) - 1);
            /* The eight slots map to modes 0,0,1,2,0,0,1,2. */
            c->im = (uint8_t)((const uint8_t[]){0, 0, 1, 2, 0, 0, 1, 2}[y]);
            return;
        default:
            switch (y) {
            case 0: c->i = c->a; c->fuel -= 1; return;
            case 1: c->r = c->a; c->fuel -= 1; return;
            case 2:                                 /* ld a,i */
            case 3:                                 /* ld a,r */
                c->a = y == 2 ? c->i : c->r;
                c->f = (uint8_t)((c->f & CF) | sz53[c->a] | (c->iff2 ? VF : 0));
                c->fuel -= 1;
                return;
            case 4: {                               /* rrd */
                uint8_t v = z80_rd(HL(c));
                z80_wr(HL(c), (uint8_t)((v >> 4) | (c->a << 4)));
                c->a = (uint8_t)((c->a & 0xF0) | (v & 0x0F));
                c->wz = (uint16_t)(HL(c) + 1);
                c->f = (uint8_t)((c->f & CF) | sz53p[c->a]);
                c->fuel -= 10;
                return;
            }
            case 5: {                               /* rld */
                uint8_t v = z80_rd(HL(c));
                z80_wr(HL(c), (uint8_t)((v << 4) | (c->a & 0x0F)));
                c->a = (uint8_t)((c->a & 0xF0) | (v >> 4));
                c->wz = (uint16_t)(HL(c) + 1);
                c->f = (uint8_t)((c->f & CF) | sz53p[c->a]);
                c->fuel -= 10;
                return;
            }
            default: return;                        /* nop */
            }
        }
    }
    if (x == 2 && z <= 3 && y >= 4) {
        int inc = (y & 1) ? -1 : 1, repeat = y >= 6;
        switch (z) {
        case 0: ldi_ldd(c, inc); c->fuel -= 8; break;
        case 1: cpi_cpd(c, inc); c->fuel -= 8; break;
        case 2: {                                   /* ini / ind */
            uint8_t v = z80_io_rd(BC(c));
            z80_wr(HL(c), v);
            c->b--;
            set_hl(c, (uint16_t)(HL(c) + inc));
            c->f = (uint8_t)(sz53[c->b] | NF);
            c->fuel -= 8;
            break;
        }
        default: {                                  /* outi / outd */
            uint8_t v = z80_rd(HL(c));
            c->b--;
            z80_io_wr(BC(c), v);
            set_hl(c, (uint16_t)(HL(c) + inc));
            c->f = (uint8_t)(sz53[c->b] | NF);
            c->fuel -= 8;
            break;
        }
        }
        /* The repeating forms re-execute by backing the PC over both prefix
         * bytes, which is also why an interrupt can land in the middle of one. */
        int again = z <= 1 ? BC(c) != 0 : c->b != 0;
        if (z == 1 && (c->f & ZF)) again = 0;       /* cpir stops on a match */
        if (repeat && again) { c->pc = (uint16_t)(c->pc - 2); c->fuel -= 5; }
        return;
    }
    /* Everything else in the ED page is a two-byte nop. */
}

static void step(Z80 *c, int xy) {
    uint8_t op = opfetch(c);
    c->fuel -= cyc_main[op];
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;

    switch (x) {
    case 0:
        switch (z) {
        case 0:
            if (y == 0) return;                              /* nop */
            if (y == 1) {                                    /* ex af,af' */
                uint8_t t = c->a; c->a = c->a2; c->a2 = t;
                t = c->f; c->f = c->f2; c->f2 = t;
                return;
            }
            if (y == 2) {                                    /* djnz */
                int8_t d = (int8_t)fetch(c);
                if (--c->b) { c->pc = (uint16_t)(c->pc + d); c->wz = c->pc; c->fuel -= 5; }
                return;
            }
            if (y == 3) {                                    /* jr */
                int8_t d = (int8_t)fetch(c);
                c->pc = (uint16_t)(c->pc + d);
                c->wz = c->pc;
                return;
            }
            {                                                /* jr cc,d */
                int8_t d = (int8_t)fetch(c);
                static const uint8_t bit[4] = { ZF, ZF, CF, CF };
                int set = (c->f & bit[y - 4]) != 0;
                if (set == ((y - 4) & 1)) {
                    c->pc = (uint16_t)(c->pc + d);
                    c->wz = c->pc;
                    c->fuel -= 5;
                }
                return;
            }
        case 1:
            if (q) add16(c, xy, p);
            else set_rp(c, p, xy, fetch16(c));
            return;
        case 2:
            switch (y) {
            case 0: z80_wr(BC(c), c->a); c->wz = (uint16_t)((c->a << 8) | ((BC(c) + 1) & 0xFF)); return;
            case 1: c->a = z80_rd(BC(c)); c->wz = (uint16_t)(BC(c) + 1); return;
            case 2: z80_wr(DE(c), c->a); c->wz = (uint16_t)((c->a << 8) | ((DE(c) + 1) & 0xFF)); return;
            case 3: c->a = z80_rd(DE(c)); c->wz = (uint16_t)(DE(c) + 1); return;
            case 4: { uint16_t nn = fetch16(c); wr16(nn, idx(c, xy)); c->wz = (uint16_t)(nn + 1); return; }
            case 5: { uint16_t nn = fetch16(c); set_idx(c, xy, rd16(nn)); c->wz = (uint16_t)(nn + 1); return; }
            case 6: { uint16_t nn = fetch16(c); z80_wr(nn, c->a); c->wz = (uint16_t)((c->a << 8) | ((nn + 1) & 0xFF)); return; }
            default: { uint16_t nn = fetch16(c); c->a = z80_rd(nn); c->wz = (uint16_t)(nn + 1); return; }
            }
        case 3:
            set_rp(c, p, xy, (uint16_t)(rp(c, p, xy) + (q ? -1 : 1)));
            return;
        case 4: {                                            /* inc r[y] */
            uint16_t ea = y == 6 ? mem(c, xy) : 0;
            set_r(c, y, y == 6 ? 0 : xy, ea, inc8(c, get_r(c, y, y == 6 ? 0 : xy, ea)));
            if (y == 6 && xy) c->fuel -= 8;      /* 23, less prefix and base */
            return;
        }
        case 5: {                                            /* dec r[y] */
            uint16_t ea = y == 6 ? mem(c, xy) : 0;
            set_r(c, y, y == 6 ? 0 : xy, ea, dec8(c, get_r(c, y, y == 6 ? 0 : xy, ea)));
            if (y == 6 && xy) c->fuel -= 8;      /* 23, less prefix and base */
            return;
        }
        case 6: {                                            /* ld r[y],n */
            uint16_t ea = y == 6 ? mem(c, xy) : 0;
            set_r(c, y, y == 6 ? 0 : xy, ea, fetch(c));
            if (y == 6 && xy) c->fuel -= 5;      /* 19, less prefix and base */
            return;
        }
        default:
            switch (y) {
            case 0: case 1: case 2: case 3: rot_a(c, y); return;
            case 4: daa(c); return;
            case 5:                                          /* cpl */
                c->a = (uint8_t)~c->a;
                c->f = (uint8_t)((c->f & (SF | ZF | VF | CF)) | HF | NF
                                 | (c->a & (YF | XF)));
                return;
            case 6:                                          /* scf */
                c->f = (uint8_t)((c->f & (SF | ZF | VF)) | CF | (c->a & (YF | XF)));
                return;
            default:                                         /* ccf */
                c->f = (uint8_t)((c->f & (SF | ZF | VF))
                    | ((c->f & CF) ? HF : 0) | ((c->f & CF) ^ CF)
                    | (c->a & (YF | XF)));
                return;
            }
        }

    case 1: {                                                /* ld r[y],r[z] */
        if (y == 6 && z == 6) {                              /* halt */
            c->halted = 1;
            c->pc--;
            return;
        }
        int mem_side = (y == 6 || z == 6);
        uint16_t ea = mem_side ? mem(c, xy) : 0;
        uint8_t v = get_r(c, z, (mem_side && z != 6) ? 0 : xy, ea);
        set_r(c, y, (mem_side && y != 6) ? 0 : xy, ea, v);
        if (mem_side && xy) c->fuel -= 8;        /* 19, less prefix and base */
        return;
    }

    case 2: {                                                /* alu[y] a,r[z] */
        uint16_t ea = z == 6 ? mem(c, xy) : 0;
        alu(c, y, get_r(c, z, z == 6 ? 0 : xy, ea));
        if (z == 6 && xy) c->fuel -= 8;          /* 19, less prefix and base */
        return;
    }

    default:
        switch (z) {
        case 0: {                                            /* ret cc */
            static const uint8_t bit[8] = { ZF, ZF, CF, CF, VF, VF, SF, SF };
            int set = (c->f & bit[y]) != 0;
            if (set == (y & 1)) { c->pc = pop(c); c->wz = c->pc; c->fuel -= 6; }
            return;
        }
        case 1:
            if (!q) { set_rp2(c, p, xy, pop(c)); return; }
            switch (p) {
            case 0: c->pc = pop(c); c->wz = c->pc; return;
            case 1: {                                        /* exx */
                uint8_t t;
                t = c->b; c->b = c->b2; c->b2 = t;
                t = c->c; c->c = c->c2; c->c2 = t;
                t = c->d; c->d = c->d2; c->d2 = t;
                t = c->e; c->e = c->e2; c->e2 = t;
                t = c->h; c->h = c->h2; c->h2 = t;
                t = c->l; c->l = c->l2; c->l2 = t;
                return;
            }
            case 2: c->pc = idx(c, xy); return;              /* jp (hl) */
            default: c->sp = idx(c, xy); return;
            }
        case 2: {                                            /* jp cc,nn */
            static const uint8_t bit[8] = { ZF, ZF, CF, CF, VF, VF, SF, SF };
            uint16_t nn = fetch16(c);
            c->wz = nn;
            if (((c->f & bit[y]) != 0) == (y & 1)) c->pc = nn;
            return;
        }
        case 3:
            switch (y) {
            case 0: c->pc = fetch16(c); c->wz = c->pc; return;
            case 1: prefix_cb(c, xy); return;
            case 2: {                                        /* out (n),a */
                uint8_t n = fetch(c);
                z80_io_wr((uint16_t)((c->a << 8) | n), c->a);
                return;
            }
            case 3: {                                        /* in a,(n) */
                uint8_t n = fetch(c);
                c->a = z80_io_rd((uint16_t)((c->a << 8) | n));
                return;
            }
            case 4: {                                        /* ex (sp),hl */
                uint16_t v = rd16(c->sp);
                wr16(c->sp, idx(c, xy));
                set_idx(c, xy, v);
                c->wz = v;
                return;
            }
            case 5: {                                        /* ex de,hl */
                uint8_t t;
                t = c->d; c->d = c->h; c->h = t;
                t = c->e; c->e = c->l; c->l = t;
                return;
            }
            case 6: c->iff1 = c->iff2 = 0; return;
            default: c->iff1 = c->iff2 = 1; c->ei_hold = 1; return;
            }
        case 4: {                                            /* call cc,nn */
            static const uint8_t bit[8] = { ZF, ZF, CF, CF, VF, VF, SF, SF };
            uint16_t nn = fetch16(c);
            c->wz = nn;
            if (((c->f & bit[y]) != 0) == (y & 1)) {
                push(c, c->pc);
                c->pc = nn;
                c->fuel -= 7;
            }
            return;
        }
        case 5:
            if (!q) { push(c, rp2(c, p, xy)); return; }
            switch (p) {
            case 0: {                                        /* call nn */
                uint16_t nn = fetch16(c);
                push(c, c->pc);
                c->pc = nn;
                c->wz = nn;
                return;
            }
            /* A prefix is a fetch of its own as far as the refresh counter is
             * concerned, and `cyc_main` has already charged its four cycles;
             * the instruction it modifies is fetched and executed next, with no
             * line logged in between. */
            case 1: step(c, 1); return;
            case 2: prefix_ed(c); return;
            default: step(c, 2); return;
            }
        case 6: alu(c, y, fetch(c)); return;
        default:                                             /* rst */
            push(c, c->pc);
            c->pc = (uint16_t)(y * 8);
            c->wz = c->pc;
            return;
        }
    }
}

/* ------------------------------------------------------------------- run -- */
static void take_irq(Z80 *c) {
    c->halted = 0;
    c->iff1 = c->iff2 = 0;
    c->r = (uint8_t)((c->r & 0x80) | ((c->r + 1) & 0x7F));
    switch (c->im) {
    case 0:
        /* Mode 0 executes whatever the device puts on the bus. Nothing on this
         * machine drives it, and the bus floats to 0xFF, which is `rst 38h` —
         * the same place mode 1 goes. */
    case 1:
        push(c, c->pc);
        c->pc = 0x0038;
        c->fuel -= 13;
        break;
    default: {
        uint16_t v = (uint16_t)((c->i << 8) | 0xFF);
        push(c, c->pc);
        c->pc = rd16(v);
        c->fuel -= 19;
        break;
    }
    }
    c->wz = c->pc;
}

unsigned z80_run(Z80 *c, int cycles) {
    c->fuel = cycles;
    while (c->fuel > 0) {
        /* EI does not let an interrupt in until the instruction after it, which
         * is what makes the `ei / ret` at the end of a handler safe. */
        int hold = c->ei_hold;
        c->ei_hold = 0;
        if (c->irq && c->iff1 && !hold) {
            take_irq(c);
            continue;
        }
        if (c->halted) {
            /* Nothing to do but burn the slice. Hardware keeps fetching NOPs
             * while it waits, so the refresh counter keeps advancing. */
            c->r = (uint8_t)((c->r & 0x80) | ((c->r + 1) & 0x7F));
            c->fuel -= 4;
            continue;
        }
        if (z80_tracing) z80_trace_line(c);
        c->insns++;
        step(c, 0);
    }
    return (unsigned)(cycles - c->fuel);
}
