/* 32X memory map and VDP, as seen by the SH-2 side.
 *
 * This replaces the flat array the recompiler tests ran against. Only the SH-2
 * view is modelled: the 68000 half of the machine is not here yet, so the few
 * places where the two CPUs rendezvous (the comm registers) are driven by the
 * host instead — see mars_set_command().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mars.h"

Mars mars;
SH2 mars_cpu[2];
jmp_buf mars_bail;
static uint32_t idle_reads, budget;
/* Which SH-2 is executing, so that a register write can tell which one did it.
 * Only the interrupt-clear registers need this; everything else in the 32X
 * register block is shared between the two by design. */
static SH2 *mars_running;

/* ---------------------------------------------------------------- helpers */
static inline uint32_t canon(uint32_t a) {
    /* Only the cached and cache-through areas are mirrors of each other; the
     * cache arrays higher up are distinct storage. */
    return a < 0x40000000u ? (a & 0x1FFFFFFFu) : a;
}

/* Every SH-2 memory access ticks this. Hooking only the VDP status register
 * was not enough: the slave's dispatch loop polls a comm byte and never touches
 * the VDP, so it spun forever in native code with nothing to unwind it. */
void mars_tick_budget(void) {
    if (++budget > 20000000u) longjmp(mars_bail, MARS_BAIL_BUDGET);
}

void mars_reset_budget(void) { budget = 0; idle_reads = 0; }

/* Both SH-2s park on a register poll that only the other CPU can end, and the
 * other CPU cannot run while this one does — so a poll that has come back zero
 * a thousand times running will never come back anything else. Unwinding is how
 * the runtime says "this one is idle now": the master waits for a command in
 * comm 0, the slave for its own command byte at 0x20004005, and both are then
 * driven by whatever the 68000 does next.
 *
 * The bound only has to be past any debounce the code does, which is two reads.
 * It used to be 200,000, which cost two million block entries of nothing every
 * time either CPU went idle. */
#define IDLE_POLLS 64
static void idle_poll(uint32_t v) {
    if (v == 0 && ++idle_reads > IDLE_POLLS) longjmp(mars_bail, MARS_BAIL_IDLE);
    if (v) idle_reads = 0;
}

static void trap(const char *what, uint32_t a) {
    if (mars.unknown++ < 16)
        fprintf(stderr, "  [mem] %s 0x%08X  (%s)\n", what, a,
                mars_running == &mars_cpu[1] ? "slave" :
                mars_running ? "master" : "no cpu");
}

/* Bit 15 of the bitmap mode register reads back set whatever is written to it.
 * The cartridge's 32X check at 0x880790 reads the register and refuses to boot
 * unless the bit is there, and with it clear the 68000 falls into the failure
 * path that ends spinning at 0x88099E.
 *
 * Two reads in the reference trace pin the behaviour down. The boot writes
 * 0x0000 to the register at 0x880730, so the bit is not simply what was
 * written; a later word read at 0x88196A returns 0x8000, and open bus cannot
 * explain it, because the word the 68000 had just prefetched there was 0x0040.
 * The byte read at 0x880790 additionally returns junk in bits 14-8, which do
 * look undriven — those are modelled as zero, which is enough for every test
 * this game makes. What the bit *means* is not identified; it appears in no
 * register description we have. */
#define BITMAP_MODE_SET 0x8000u

/* The SH-2 spends much of the boot polling hardware. Nothing here advances on
 * its own, so a free-running counter stands in for the passage of time and
 * lets those loops terminate. */
static uint16_t vdp_status(void) {
    uint32_t t = mars.ticks++;
    uint16_t s = 0;
    if ((t & 0x3F) < 0x08) s |= 0x8000;      /* VBLK */
    if ((t & 0x07) < 0x02) s |= 0x4000;      /* HBLK */
    s |= mars.fbctl & 0x0003;                /* FEN / FS as written */
    s |= 0x2000;                             /* PEN: palette accessible */
    return s;
}

/* --- the DREQ FIFO and the one DMA transfer that uses it -------------------
 *
 * The 68000 hands the SH-2 its command list like this (0x8819AE onward):
 * set the word count at 0xA15110, set the DREQ control byte, raise the master's
 * command interrupt, spin until the handler clears it, then push the payload
 * word by word into the FIFO at 0xA15112.
 *
 * The handler it wakes — 0x06001334, reached through the dispatcher at
 * 0x060001B0 — arms SH-2 DMAC channel 0 with SAR0 = 0x20004012 (the FIFO read
 * port, address fixed), DAR0 = the list buffer in SDRAM, and TCR0 = the count
 * it reads back from 0x20004010. So the channel is always armed *before* the
 * data arrives, and modelling it as "consume on push" is exact rather than a
 * simplification. No general DMAC is needed for it, and pretending otherwise
 * would mean modelling channel arbitration nothing here uses.
 */
#define DMAC_SAR0  0xFFFFFF80u
#define DMAC_DAR0  0xFFFFFF84u
#define DMAC_TCR0  0xFFFFFF88u
#define DMAC_CHCR0 0xFFFFFF8Cu
#define DMAC_DMAOR 0xFFFFFFB0u
#define DREQ_FIFO  0x20004012u

static uint8_t *resolve(uint32_t a, uint32_t need);

static uint32_t oc32(uint32_t a) {
    const uint8_t *p = &mars.onchip[a - 0xFFFFFE00u];
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}

static void oc32w(uint32_t a, uint32_t v) {
    uint8_t *p = &mars.onchip[a - 0xFFFFFE00u];
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void dma_drain(void) {
    while (mars.fifo_n) {
        uint32_t chcr = oc32(DMAC_CHCR0), tcr = oc32(DMAC_TCR0);
        /* DMAOR's master enable, the channel's own DE bit, something left to
         * transfer, and a source that is actually this FIFO. */
        if (!(oc32(DMAC_DMAOR) & 1) || !(chcr & 1) || !tcr) return;
        if (oc32(DMAC_SAR0) != DREQ_FIFO) return;
        uint32_t dar = oc32(DMAC_DAR0);
        uint16_t w = mars.fifo[0];
        /* Written through `resolve` rather than sh2_w16: this runs inside a
         * 68000 register write, where the budget watchdog's longjmp would
         * unwind straight out of the interpreter. */
        uint8_t *p = resolve(canon(dar), 2);
        if (!p) { trap("dma", dar); return; }
        p[0] = (uint8_t)(w >> 8); p[1] = (uint8_t)w;
        memmove(mars.fifo, mars.fifo + 1, --mars.fifo_n * sizeof *mars.fifo);
        oc32w(DMAC_DAR0, dar + 2);
        oc32w(DMAC_TCR0, tcr - 1);
        mars.dma_words++;
        if (tcr == 1) oc32w(DMAC_CHCR0, (chcr & ~1u) | 2u);   /* DE off, TE on */
    }
}

static void fifo_push(uint16_t w) {
    if (mars.fifo_n < sizeof mars.fifo / sizeof *mars.fifo)
        mars.fifo[mars.fifo_n++] = w;
    dma_drain();
}

static void autofill(void) {
    uint32_t addr = mars.fill_start;
    for (uint32_t i = 0; i <= mars.fill_len; i++) {
        uint32_t o = (addr & 0x1FFFFu);
        mars_fb_draw()[o] = (uint8_t)(mars.fill_data >> 8);
        mars_fb_draw()[o + 1] = (uint8_t)mars.fill_data;
        /* The fill wraps within a 512-byte line, which is what makes it
         * useful for clearing one scanline at a time. */
        addr = (addr & 0xFF00u) | ((addr + 2) & 0xFFu);
    }
}

/* ------------------------------------------------------------- dispatch ---
 * Returns 1 if the access was handled as a register, 0 if it is plain memory.
 */
int mars_reg_write_sh2(uint32_t a, uint32_t v, int size) {
    if (a >= 0x4000 && a < 0x4100) {          /* system registers */
        switch (a & 0xFE) {
        case 0x00: mars.adapter = (uint16_t)v; return 1;
        case 0x02: mars.intctl = (uint16_t)v; return 1;
        case 0x04: mars.bank = (uint16_t)v; return 1;
        case 0x06: mars.dreq_ctl = (uint16_t)v; return 1;
        case 0x10: mars.dreq_len = (uint16_t)v; return 1;
        case 0x12: fifo_push((uint16_t)v); return 1;   /* only the 68000 writes */
        case 0x1A:                            /* command interrupt clear */
            /* The 68000 spins on this bit waiting for the handler to say it has
             * armed its DMAC. Which CPU is clearing it is whichever one the
             * runtime is currently executing. */
            mars.intctl &= (uint16_t)~(mars_running == &mars_cpu[1] ? 2u : 1u);
            return 1;
        case 0x14: case 0x16: case 0x18:
        case 0x1C: case 0x1E: return 1;       /* the other interrupt clears */
        case 0x30: mars.pwm_ctl = (uint16_t)v; return 1;
        case 0x32: mars.pwm_cycle = (uint16_t)v; return 1;
        case 0x20:                            /* comm 0: the 68000 rendezvous */
            /* Plain storage now. The SH-2 zeroing this is how it tells the
             * 68000 it is ready, and the 68000 waits for exactly that — at
             * 0x8845CE among other places. It used to be intercepted, because
             * commands arrived a whole frame before the SH-2 ran and would have
             * been wiped by the zero the dispatch loop writes on entry; the
             * 68000 now drives the SH-2 at the moment it posts, so there is
             * nothing left to paper over. */
            mars.comm[0] = (uint16_t)v;
            return 1;
        default:
            if ((a & 0xFE) >= 0x20 && (a & 0xFE) < 0x30) {
                mars.comm[((a & 0xFE) - 0x20) / 2] = (uint16_t)v;
                return 1;
            }
            return 1;
        }
    }
    if (a >= 0x4100 && a < 0x4200) {          /* VDP registers */
        switch (a & 0xFE) {
        case 0x00: mars.bitmap_mode = (uint16_t)v & ~BITMAP_MODE_SET; return 1;
        case 0x02: mars.shift = (uint16_t)v; return 1;
        case 0x04: mars.fill_len = (uint16_t)v; return 1;
        case 0x06: mars.fill_start = (uint16_t)v; return 1;
        case 0x08: mars.fill_data = (uint16_t)v; autofill(); return 1;
        case 0x0A: mars.fbctl = (uint16_t)v; return 1;
        default: return 1;
        }
    }
    if (a >= 0x4200 && a < 0x4400) {          /* palette */
        uint32_t o = a - 0x4200;
        if (size == 1) mars.cram[o] = (uint8_t)v;
        else { mars.cram[o] = (uint8_t)(v >> 8); mars.cram[o + 1] = (uint8_t)v; }
        return 1;
    }
    return 0;
}

int mars_reg_read_sh2(uint32_t a, uint32_t *out) {
    if (a >= 0x4000 && a < 0x4100) {
        switch (a & 0xFE) {
        case 0x00: *out = mars.adapter; return 1;
        case 0x02: *out = mars.intctl; return 1;
        case 0x04:
            *out = mars.bank;
            /* The slave's own command byte lives in the low half and it polls
             * it forever; the master reads the same register as a bank select
             * and must not be unwound for it. */
            if (mars_running == &mars_cpu[1]) idle_poll(*out & 0xFF);
            return 1;
        case 0x06: *out = mars.dreq_ctl; return 1;
        case 0x10: *out = mars.dreq_len; return 1;
        case 0x30: *out = mars.pwm_ctl; return 1;
        case 0x32: *out = mars.pwm_cycle; return 1;
        /* The sample FIFOs. Bits 15 and 14 are the full and empty flags; both
         * clear says "room for more", which is what keeps the driver from
         * waiting on an audio sink that does not exist yet. */
        case 0x34: case 0x36: case 0x38: *out = 0; return 1;
        case 0x12:                            /* the FIFO read port */
            /* The DMAC drains the FIFO directly, so nothing here reads it in
             * practice; answering honestly costs nothing and means a CPU that
             * polls the port instead sees the same data. */
            *out = mars.fifo_n ? mars.fifo[0] : 0;
            if (mars.fifo_n)
                memmove(mars.fifo, mars.fifo + 1,
                        --mars.fifo_n * sizeof *mars.fifo);
            return 1;
        default:
            if ((a & 0xFE) >= 0x20 && (a & 0xFE) < 0x30) {
                *out = mars.comm[((a & 0xFE) - 0x20) / 2];
                /* Waiting on a command the 68000 cannot send while we run. */
                if ((a & 0xFE) == 0x20) idle_poll(*out);
                return 1;
            }
            *out = 0; return 1;
        }
    }
    if (a >= 0x4100 && a < 0x4200) {
        switch (a & 0xFE) {
        case 0x00: *out = mars.bitmap_mode | BITMAP_MODE_SET; return 1;
        case 0x02: *out = mars.shift; return 1;
        case 0x0A: *out = vdp_status(); return 1;
        default: *out = 0; return 1;
        }
    }
    if (a >= 0x4200 && a < 0x4400) {
        uint32_t o = a - 0x4200;
        *out = ((uint32_t)mars.cram[o] << 8) | mars.cram[o + 1];
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- memory --- */
#define IN(a, lo, hi) ((a) >= (lo) && (a) < (hi))

static uint8_t *resolve(uint32_t a, uint32_t need) {
    if (IN(a, 0x06000000u, 0x06040000u)) return &mars.sdram[a - 0x06000000u];
    if (IN(a, 0x04000000u, 0x04020000u)) return mars_fb_draw() + (a - 0x04000000u);
    if (IN(a, 0x04020000u, 0x04040000u)) return mars_fb_draw() + (a - 0x04020000u);
    if (IN(a, 0x02000000u, 0x02000000u + MARS_ROM_MAX)) {
        uint32_t o = a - 0x02000000u;
        return o + need <= mars.rom_size ? &mars.rom[o] : NULL;
    }
    if (IN(a, 0xC0000000u, 0xC0001000u)) return &mars.cache[a - 0xC0000000u];
    /* SH-2 on-chip peripherals: the free-running timer, watchdog, DMAC and
     * serial block. The boot code programs the FRT and never reads anything
     * back that it did not write, so backing them with plain storage is
     * enough to get through init. */
    if (a >= 0xFFFFFE00u) return &mars.onchip[a - 0xFFFFFE00u];
    return NULL;
}

/* The overwrite image drops zero bytes instead of storing them, which is how
 * sprites are drawn without a mask. */
static int is_overwrite(uint32_t a) {
    return IN(a, 0x04020000u, 0x04040000u);
}

/* The SH7604 splits its address space by the top three bits, and 0x40000000 to
 * 0x5FFFFFFF is the associative purge area: a write there invalidates the cache
 * line holding the same address in the cached area, and the data is discarded.
 * Our SH-2 has no cache to purge, so these are no-ops — but they have to be
 * recognised, or the game's cache management shows up as 1,792 unmapped writes
 * to addresses like 0x46004F82, which is SDRAM 0x06004F82 being purged. */
static int is_purge(uint32_t a) {
    return IN(a, 0x40000000u, 0x60000000u);
}

uint8_t sh2_r8(SH2 *c, uint32_t a) {
    mars_tick_budget();
    (void)c; a = canon(a);
    uint32_t v;
    if (a < 0x10000u && mars_reg_read_sh2(a, &v)) return (uint8_t)((a & 1) ? v : v >> 8);
    uint8_t *p = resolve(a, 1);
    if (p) return *p;
    trap("r8", a);
    return 0;
}
uint16_t sh2_r16(SH2 *c, uint32_t a) {
    mars_tick_budget();
    (void)c; a = canon(a);
    uint32_t v;
    if (a < 0x10000u && mars_reg_read_sh2(a, &v)) return (uint16_t)v;
    uint8_t *p = resolve(a, 2);
    if (p) return (uint16_t)((p[0] << 8) | p[1]);
    trap("r16", a);
    return 0;
}
uint32_t sh2_r32(SH2 *c, uint32_t a) {
    mars_tick_budget();
    (void)c; a = canon(a);
    if (a < 0x10000u) {
        uint32_t hi, lo;
        if (mars_reg_read_sh2(a, &hi) && mars_reg_read_sh2(a + 2, &lo)) return (hi << 16) | lo;
    }
    uint8_t *p = resolve(a, 4);
    if (p) return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                | ((uint32_t)p[2] << 8) | p[3];
    trap("r32", a);
    return 0;
}
void sh2_w8(SH2 *c, uint32_t a, uint8_t v) {
    mars_tick_budget();
    (void)c; uint32_t ra = canon(a);
    if (is_purge(ra)) return;
    if (ra < 0x10000u && mars_reg_write_sh2(ra, v, 1)) return;
    if (is_overwrite(ra) && v == 0) return;
    uint8_t *p = resolve(ra, 1);
    if (p) { if (ra < 0x02000000u || ra >= 0x03000000u) *p = v; return; }
    trap("w8", ra);
}
void sh2_w16(SH2 *c, uint32_t a, uint16_t v) {
    mars_tick_budget();
    (void)c; uint32_t ra = canon(a);
    if (is_purge(ra)) return;
    if (ra < 0x10000u && mars_reg_write_sh2(ra, v, 2)) return;
    uint8_t *p = resolve(ra, 2);
    if (p) {
        if (is_overwrite(ra)) {
            if (v >> 8) p[0] = (uint8_t)(v >> 8);
            if (v & 0xFF) p[1] = (uint8_t)v;
        } else { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
        return;
    }
    trap("w16", ra);
}
void sh2_w32(SH2 *c, uint32_t a, uint32_t v) {
    sh2_w16(c, a, (uint16_t)(v >> 16));
    sh2_w16(c, a + 2, (uint16_t)v);
}

/* ------------------------------------------------------------ transfers --- */
typedef struct { uint32_t addr; uint32_t (*fn)(SH2 *, uint32_t); } SH2Entry;
extern const SH2Entry sh2_functions[];
extern const unsigned sh2_function_count;

/* --- tracing -------------------------------------------------------------
 * Every function entry goes through the dispatch loop below, tail transfers
 * included, so the trace lives entirely here and the generated code carries no
 * instrumentation at all. Inferring control flow from final register values
 * had stopped being productive; this records what actually ran.
 */
#define TRACE_RING 8192
#define TRACE_MAXFN 1024
static struct { uint32_t addr; uint16_t depth; uint8_t tail; } tring[TRACE_RING];
static unsigned tring_n;                       /* total entries, may exceed ring */
static uint32_t tcount[TRACE_MAXFN];

void mars_trace_reset(void) { tring_n = 0; memset(tcount, 0, sizeof tcount); }

static void trace_enter(uint32_t addr, unsigned depth, int tail) {
    tring[tring_n % TRACE_RING].addr = addr;
    tring[tring_n % TRACE_RING].depth = (uint16_t)depth;
    tring[tring_n % TRACE_RING].tail = (uint8_t)tail;
    tring_n++;
}

void mars_trace_dump(const char *why) {
    fprintf(stderr, "\n== trace: %s ==\n", why);
    /* A spin shows up as a repeating run of addresses in the entry ring. */
    unsigned have = tring_n < TRACE_RING ? tring_n : TRACE_RING;
    for (unsigned period = 1; period <= 12 && period * 3 <= have; period++) {
        int same = 1;
        for (unsigned k = 0; k < period * 2 && same; k++)
            if (tring[(tring_n - 1 - k) % TRACE_RING].addr !=
                tring[(tring_n - 1 - k - period) % TRACE_RING].addr) same = 0;
        if (same) {
            fprintf(stderr, "  spinning on %u address(es):", period);
            for (unsigned k = 0; k < period; k++)
                fprintf(stderr, " 0x%08X",
                        tring[(tring_n - period + k) % TRACE_RING].addr);
            fprintf(stderr, "\n");
            break;
        }
    }

    unsigned shown = tring_n < 16 ? tring_n : 16;
    fprintf(stderr, "last %u block entries:\n", shown);
    for (unsigned i = tring_n - shown; i < tring_n; i++)
        fprintf(stderr, "   0x%08X\n", tring[i % TRACE_RING].addr);

    /* Hottest functions, which is usually where a runaway loop lives. */
    unsigned top[5] = {0}, ntop = 0;
    for (unsigned i = 0; i < sh2_function_count && i < TRACE_MAXFN; i++) {
        if (!tcount[i]) continue;
        unsigned j = ntop < 5 ? ntop++ : 5;
        while (j > 0 && tcount[top[j - 1]] < tcount[i]) { if (j < 5) top[j] = top[j-1]; j--; }
        if (j < 5) top[j] = i;
    }
    fprintf(stderr, "most-entered:\n");
    for (unsigned i = 0; i < ntop; i++)
        fprintf(stderr, "   0x%08X  x%u\n",
                sh2_functions[top[i]].addr, tcount[top[i]]);
    fprintf(stderr, "total entries: %u\n", tring_n);
}

/* --- block-entry trace ---------------------------------------------------
 * One line per basic block entered, carrying the whole register state, written
 * in the shape the reference tracer uses for its own SHM/SHS lines so that
 * `tools/diffsh2.py` parses both with one reader. The reference logs every
 * instruction; filtered to the addresses in `sh2_functions[]` its stream is
 * exactly this one, which is what makes the two comparable without the
 * generated code having to carry a hook per instruction.
 *
 * The flag string is the reference's: M, Q, the interrupt mask as one hex
 * digit, S, T — upper case for set. It is redundant with `sr`, and emitted
 * anyway so a line of ours and a line of theirs are the same shape.
 */
int sh2_trace;
static FILE *sh2_trace_f;
/* Budgeted per CPU. The slave's idle handler is a tight poll that only the
 * watchdog stops, so one shared budget would be spent entirely on it and the
 * master — the half with the interesting init — would never be recorded. */
static unsigned long sh2_trace_left[2];

int sh2_trace_open(const char *path, unsigned long max_lines) {
    sh2_trace_f = fopen(path, "w");
    if (!sh2_trace_f) { perror(path); return 0; }
    sh2_trace_left[0] = sh2_trace_left[1] = max_lines;
    sh2_trace = 1;
    return 1;
}

void sh2_trace_close(void) {
    if (sh2_trace_f) fclose(sh2_trace_f);
    sh2_trace_f = NULL;
    sh2_trace = 0;
}

void sh2_block(SH2 *c, uint32_t addr) {
    if (!sh2_trace_f) return;
    if (!sh2_trace_left[c->slave & 1]) return;
    sh2_trace_left[c->slave & 1]--;
    fprintf(sh2_trace_f,
            "%s  %08x  r0:%08x r1:%08x r2:%08x r3:%08x r4:%08x r5:%08x "
            "r6:%08x r7:%08x r8:%08x r9:%08x r10:%08x r11:%08x r12:%08x "
            "r13:%08x r14:%08x r15:%08x sr:%08x gbr:%08x vbr:%08x mach:%08x "
            "macl:%08x pr:%08x %c%c%x%c%c\n",
            c->slave ? "SHS" : "SHM", addr,
            c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6],
            c->r[7], c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13],
            c->r[14], c->r[15], sh2_get_sr(c), c->gbr, c->vbr, c->mach,
            c->macl, c->pr,
            c->m ? 'M' : 'm', c->q ? 'Q' : 'q', c->imask & 0xF,
            c->s ? 'S' : 's', c->t ? 'T' : 't');
}

/* The table is sorted by address, so binary search it. It lists every basic
 * block, not just function entries: a return goes to the instruction after a
 * call, which is mid-function. */
static int lookup(uint32_t addr) {
    unsigned lo = 0, hi = sh2_function_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (sh2_functions[mid].addr == addr) return (int)mid;
        if (sh2_functions[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return -1;
}

void sh2_call(SH2 *c, uint32_t addr) {
    /* Flat: calls, returns and jumps are all just a new address. Nothing
     * recurses, so a handler that branches away instead of returning - which
     * is ordinary SH-2 practice - costs nothing here either. */
    SH2 *outer = mars_running;
    mars_running = c;
    while (addr) {
        if (addr < 0x40000000u) addr &= 0x1FFFFFFFu;
        int i = lookup(addr);
        if (i < 0) {
            if (mars.missing++ < 16)
                fprintf(stderr, "  [call] no recompiled block at 0x%08X\n", addr);
            break;
        }
        if (mars.trace) {
            trace_enter(addr, 0, 0);
            if (i < TRACE_MAXFN) tcount[i]++;
        }
        mars_tick_budget();
        addr = sh2_functions[i].fn(c, addr);
    }
    mars_running = outer;
}

/* Take an external interrupt and run the handler to completion.
 *
 * The SH-2 takes external interrupts through auto-vectors 64-71, two levels to
 * a vector, so the command interrupt at level 8 lands on vector 68. Both of
 * this cartridge's tables fill all eight with one dispatcher — 0x060001B0 on
 * the master, 0x060001F8 on the slave — which reads the accepted level back out
 * of SR to pick the real handler. That is why the mask has to be set before
 * entering, and put back afterwards.
 *
 * Nothing is pushed on the SH-2 stack: `rte` returns to the runtime here rather
 * than popping a frame, and the dispatcher balances its own pushes.
 */
void mars_deliver_int(int slave, unsigned level) {
    SH2 *c = &mars_cpu[slave & 1];
    uint32_t vec = c->vbr + 4u * (64u + ((level + 1u) >> 1));
    const uint8_t *p = resolve(canon(vec), 4);
    if (!p) { trap("vector", vec); return; }
    uint32_t handler = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                     | ((uint32_t)p[2] << 8) | p[3];
    if (!handler) return;

    uint32_t save = sh2_get_sr(c);
    c->imask = level & 0xF;
    /* Its own setjmp: this runs inside a 68000 register write, and the
     * watchdog's unwind must not escape into the interpreter. The frame loop
     * re-arms the buffer before each dispatch, so borrowing it is safe. */
    mars_reset_budget();
    int why = setjmp(mars_bail);
    if (why == 0) sh2_call(c, handler);
    else mars.bail[why & 3]++;
    sh2_set_sr(c, save);
}

/* Run the master's command dispatch loop, entered at its poll rather than at
 * its head: the head writes 0 to comm 0 first, which would wipe the command the
 * 68000 has just put there. From the poll it reads the command, dispatches it,
 * and comes back round to the head — where the 0 it writes is the
 * acknowledgement the 68000 is waiting for. */
#define MASTER_POLL 0x060008F8u

/* The PWM timer, which is the only clock the slave has.
 *
 * Its sound driver programs the two registers from 0xC0000008 and then idles in
 * a delay loop; every PWM interrupt is one sample period, and nothing else ever
 * wakes it. So the rate is the machine's own rather than a number picked to
 * look right: the SH-2 clock divided by the programmed cycle length, divided
 * again by TM, the interrupt-every-N-cycles field.
 *
 * This game writes cycle 0x417 and TM 1, which is 21,957 Hz and 366 interrupts
 * to a 60 Hz frame. The reference logs 4,877 of them against 13 of the 68000's,
 * or 375 apiece — the same number, measured the other way round.
 */
#define SH2_CLOCK 23011360u              /* NTSC 32X */

unsigned mars_pwm_per_frame(void) {
    unsigned tm = (mars.pwm_ctl >> 8) & 0xF;
    unsigned cycle = mars.pwm_cycle & 0xFFF;
    if (!tm || !cycle) return 0;         /* the timer interrupt is off */
    return SH2_CLOCK / ((cycle + 1) * tm) / 60;
}

void mars_run_command(void) {
    mars_reset_budget();
    int why = setjmp(mars_bail);
    if (why == 0) sh2_call(&mars_cpu[0], MASTER_POLL);
    else mars.bail[why & 3]++;
    mars.serviced++;
}

void sh2_unimplemented(SH2 *c, uint32_t addr, const char *what) {
    (void)c;
    fprintf(stderr, "  [sh2] unimplemented at 0x%08X: %s\n", addr, what);
}

