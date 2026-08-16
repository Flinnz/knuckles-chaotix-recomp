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
#include "sound.h"

Mars mars;
SH2 mars_cpu[2];
int trace_armed = 1;
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

/* There were two watchdogs here: a budget that unwound after 20 million memory
 * accesses, and an idle-poll bound that unwound after 64 reads of a register
 * only the other CPU could change. Both existed for one reason — an SH-2 ran to
 * completion, so a CPU waiting on the other could only be stopped by abandoning
 * its call outright.
 *
 * Slicing removes the premise rather than tuning the bound. A CPU parked on a
 * poll now spends its slice polling and yields at a block boundary with its
 * position intact, which is what the hardware does; the other CPU then runs and
 * the poll ends on its own. See SH2_BLOCK in src/sh2.h.
 */

/* Set by the dispatch loop below. An unmapped access is nearly always a pointer
 * that should have been filled in, and the function it happened in is what says
 * which one — the address alone does not. */
static uint32_t trap_fn;

static void trap(const char *what, uint32_t a) {
    if (mars.unknown++ < 16)
        fprintf(stderr, "  [mem] %s 0x%08X  in 0x%08X  (%s)\n", what, a, trap_fn,
                mars_running == &mars_cpu[1] ? "slave" :
                mars_running ? "master" : "no cpu");
}

/* A watchpoint on the SH-2s' address space.
 *
 * The tables the 32X draws from are built by one routine and read by another,
 * frames apart, so "which entry is wrong" and "who wrote it" are different
 * questions and only the second one is answerable by reading code. `trap_fn` is
 * the last address the dispatch loop transferred to, which is the block the
 * write is inside — not the exact PC, and enough to name the routine. */
uint32_t mars_watch_lo, mars_watch_hi;

/* Every distinct value the game has put in the bitmap mode register, as a bit
 * per mode. The register decides both what the frame buffer *means* — packed
 * pixel, direct colour, run length — and which half of the picture is in
 * front, and only two of the four modes have ever been exercised by anything
 * this project has run. A screen that draws nothing from the 32X is asking
 * this question first. */
uint8_t mars_modes_seen;
static unsigned watch_n;

static void watch(const char *what, uint32_t a, uint32_t v) {
    if (a < mars_watch_lo || a >= mars_watch_hi) return;
    if (watch_n++ >= 200) return;
    fprintf(stderr, "  [watch] %s 0x%08X = 0x%X  in 0x%08X  (%s)\n",
            what, a, v, trap_fn,
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
/* The 32X VDP's frame buffer control register, which is also its status.
 *
 * VBLK and HBLK are the beam, and they are the *same* beam the Mega Drive side
 * reports — one adapter, one display. They used to be a free-running counter
 * here, set for 8 reads in every 64, which is what the Genesis side had before
 * it got a scanline clock and what nobody came back for. It is not a cosmetic
 * difference: the master synchronises its whole video init to this bit — at
 * 0x06003116 it waits for VBLK to clear and then to set, which is "wait for the
 * next vertical blank" — and against a counter that flips every eight reads it
 * waited a few instructions where the reference waits 192,761. Everything the
 * master then does was that much early, which is why the 68000 found comm 0
 * free at vblanks where the reference finds it busy.
 *
 * FEN is the fill, and the reference measures it from both sides. The master's
 * clear loop at 0x06003188 starts an autofill and waits for FEN to drop, and
 * that wait is **79,576 instructions**, a third of a frame — while at
 * 0x06003106, where no fill is outstanding, the same register is read once and
 * the code walks straight past it. Nothing about a 256-word fill takes a third
 * of a frame; what takes a third of a frame is waiting for the display period
 * to end. So the fill is *performed* in the blanking interval, and FEN is "a
 * fill is outstanding" — set when the data register is written during display,
 * cleared when the beam reaches the vertical blank. Modelling it as the display
 * period itself was tried and is wrong in exactly the way 0x06003106 shows: it
 * stalls 197,660 instructions where the reference runs 4.
 *
 * It was `mars.fbctl & 2` — the bit the CPU had written, which is never set —
 * so the wait was 2,550 instructions instead of 78,000, and the master's video
 * init ran a whole frame early.
 *
 * PEN stays permissive. On hardware the palette is only writable outside the
 * display period; nothing here polls it, and answering "always accessible"
 * cannot stall a game that does not ask.
 */
static uint16_t vdp_status(void) {
    uint16_t s = 0;
    if (gen.line >= 224) s |= 0x8000;        /* VBLK */
    /* HBLK reads set through the vertical blank as well as the horizontal one,
     * which is what the reference's two samples of this register show: 0xE000
     * with the beam off the display and 0x2000 with it on. */
    if (gen.hpos >= 192 || gen.line >= 224) s |= 0x4000;
    if (mars.fen_left) s |= 0x0002;          /* FEN: a fill is still running */
    s |= mars.fbctl & 0x0001;                /* FS, which really is the CPU's */
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

/* --- the SH7604's on-chip divide unit --------------------------------------
 *
 * The SH-2 has no divide instruction. It has this instead: write the divisor to
 * DVSR and the dividend to DVDNT, and thirty-nine cycles later DVDNT holds the
 * quotient and DVDNTH the remainder. So every division this game does goes
 * through four longword accesses to 0xFFFFFF00, and the on-chip block was plain
 * storage — which returns the *dividend* where the quotient should be.
 *
 * That is not a subtle wrongness. The master's polygon clipper at 0x060048F4
 * interpolates a clipped vertex as `(a * b) / c`, and without the divide it
 * gets `a * b`: the right shape at thousands of times the scale. At frame 429,
 * where the game leaves the SEGA logo for the title screen, it hands the
 * rasteriser a quad 20,670 rows tall with spans 53,000 pixels wide, and since
 * every row of a span is one 32X autofill the master waits on, one polygon
 * takes about twenty-seven frames. The 68000 then spends whole frames in the
 * comm-0 acknowledgement wait at 0x8845CE, which is why nothing else advanced.
 *
 * Four literal pools in the master's image hold 0xFFFFFF00 — 0x06001FD0,
 * 0x060026A4, 0x06005BB8 and 0x06005C20 — so this is four routines, not one.
 *
 * Signed throughout, and C's truncation toward zero is the SH-2's own rule.
 * A 32-bit write to DVDNT sign-extends into the 64-bit dividend; a write to
 * DVDNTL divides whatever DVDNTH:DVDNTL holds. Overflow and divide-by-zero set
 * DVCR's OVF bit and saturate, which is what the manual specifies; nothing in
 * this game reads either, so they are here to be right rather than because
 * anything depends on them.
 */
#define DVSR   0xFFFFFF00u
#define DVDNT  0xFFFFFF04u
#define DVCR   0xFFFFFF08u
#define DVDNTH 0xFFFFFF10u
#define DVDNTL 0xFFFFFF14u

static void oc32w(uint32_t a, uint32_t v);
static uint32_t oc32(uint32_t a);

static void divu_run(void) {
    int32_t dvsr = (int32_t)oc32(DVSR);
    int64_t dvd = (int64_t)(((uint64_t)oc32(DVDNTH) << 32) | oc32(DVDNTL));
    int64_t q, r = 0;
    if (!dvsr || (dvd == INT64_MIN && dvsr == -1)) {
        q = dvd < 0 ? INT32_MIN : INT32_MAX;
        oc32w(DVCR, oc32(DVCR) | 1u);
    } else {
        q = dvd / dvsr;
        r = dvd % dvsr;
        if (q > INT32_MAX || q < INT32_MIN) {
            q = dvd < 0 ? INT32_MIN : INT32_MAX;
            oc32w(DVCR, oc32(DVCR) | 1u);
        }
    }
    oc32w(DVDNT, (uint32_t)(int32_t)q);
    oc32w(DVDNTL, (uint32_t)(int32_t)q);
    oc32w(DVDNTH, (uint32_t)(int32_t)r);
}

/* A longword write into the divider's block. Returns 1 when it was one. */
static int divu_w32(uint32_t a, uint32_t v) {
    if (a != DVDNT && a != DVDNTL) return 0;
    if (a == DVDNT) {
        oc32w(DVDNTL, v);
        oc32w(DVDNTH, (uint32_t)((int32_t)v >> 31));   /* sign-extended */
    } else {
        oc32w(DVDNTL, v);
    }
    divu_run();
    return 1;
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
         * 68000 register write, with no SH-2 executing, and sh2_w16 wants the
         * CPU whose address space it is. */
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

/* 68S stays set for exactly as many words as the length register asked for, and
 * the adapter drops it on the last one. That is what the 68000 reads back at
 * 0x883250 while it feeds the FIFO four words at a time: 0x04 for as long as the
 * loop still has a group to push, and 0 on the read after the final group —
 * which is the reference's flags at 0x883254 and at 0x88325A respectively. */
static void fifo_push(uint16_t w) {
    if (mars.fifo_n < sizeof mars.fifo / sizeof *mars.fifo)
        mars.fifo[mars.fifo_n++] = w;
    if (mars.dreq_left && !--mars.dreq_left)
        mars.dreq_ctl &= (uint16_t)~0x0004u;
    dma_drain();
}

/* The fill start register holds a *word* address, not a byte one.
 *
 * Sixteen bits reach all 65,536 words of the 128 KB frame buffer exactly, which
 * is the argument: as a byte address it would reach half of it. Taking it for a
 * byte offset put every fill at half the address it belonged at, and what that
 * landed on was the line table — the game fills from word 0x100 up, we wrote
 * from byte 0x100, and 96 of the 224 line-table entries the master had just
 * written were zeroed under it. That is the "96 of the 224 lines have no
 * line-table entry" this project carried as a stable property of the picture
 * for as long as it has had one.
 *
 * The address also increments in its low 8 bits only, so a fill wraps inside a
 * 256-word block — which is what makes it useful for clearing one scanline of a
 * 512-byte-stride buffer, and which stepping a byte address by two did not do.
 */
static void autofill(void) {
    /* The fill itself happens here and now; what is modelled is how long the
     * VDP holds the frame buffer while doing it, because the master waits for
     * exactly that. Two SH-2 cycles a word — see vdp_status(). */
    mars.fen_left = (mars.fill_len + 1u) * 2u;
    mars.fills++;
    mars.fill_words += mars.fill_len + 1u;
    uint32_t addr = mars.fill_start;
    for (uint32_t i = 0; i <= mars.fill_len; i++) {
        uint32_t o = (addr * 2u) & 0x1FFFEu;
        mars_fb_draw()[o] = (uint8_t)(mars.fill_data >> 8);
        mars_fb_draw()[o + 1] = (uint8_t)mars.fill_data;
        addr = (addr & 0xFF00u) | ((addr + 1) & 0xFFu);
    }
}

/* ------------------------------------------------------------- dispatch ---
 * Returns 1 if the access was handled as a register, 0 if it is plain memory.
 */
int mars_reg_write_sh2(uint32_t a, uint32_t v, int size) {
    if (a >= 0x4000 && a < 0x4100) {          /* system registers */
        /* The comm registers are sixteen bits and this game writes them a byte
         * at a time, so the half not addressed has to survive.
         *
         * It is the special stage's sound. The master posts a sound id into the
         * *high* byte of comm 0 with `mov.b`, and the 68000's poll at 0x9279E2
         * reads the whole word, takes the high byte as the id and the low byte
         * as "there is something here" — the low byte being whatever command
         * was last posted, 0x02 for the duration of a special stage. Storing the
         * byte as the whole word cleared that flag, so the poll saw nothing and
         * returned -1, and every one of those sounds was dropped: 30 of them in
         * the 2,500 frames measured, which is a special stage with no effects at
         * all and only the music the Z80 keeps playing on its own.
         *
         * Reads have always taken the addressed half (`sh2_r8`); this is the
         * write side of the same thing. The rest of the block is left alone —
         * a byte write to the FIFO port or an interrupt clear is not something
         * this game does, and merging one would mean inventing what it means. */
        if ((a & 0xFE) >= 0x20 && (a & 0xFE) < 0x30) {
            unsigned ci = (unsigned)(((a & 0xFE) - 0x20) / 2);
            uint16_t old = mars.comm[ci];
            uint16_t nv = size == 1
                ? (uint16_t)((a & 1) ? (old & 0xFF00) | (v & 0xFF)
                                     : (old & 0x00FF) | ((v & 0xFF) << 8))
                : (uint16_t)v;
            /* The master zeroing comm 0 is the acknowledgement the 68000 waits
             * for, so it is the one honest count of commands actually run —
             * `serviced` used to be incremented by the 68000's own post, which
             * only measured that a command had been asked for. */
            if (!ci && !nv && old && mars_running == &mars_cpu[0])
                mars.serviced++;
            mars.comm[ci] = nv;
            return 1;
        }
        switch (a & 0xFE) {
        case 0x00:
            mars.adapter = (uint16_t)v;
            /* The low byte is the interrupt enable mask and it belongs to the
             * SH-2 that wrote it, not to the adapter: the slave enables PWM and
             * the master enables V, and sharing one field means each turns the
             * other off. Only an SH-2's own write counts; the 68000 reaches
             * this block too and has no mask of its own. */
            if (mars_running)
                mars.int_enable[mars_running == &mars_cpu[1]] = (uint8_t)v;
            return 1;
        case 0x02: mars.intctl = (uint16_t)v; return 1;
        /* H Count, not the 68000's bank register — see the note in mars.h. */
        case 0x04: mars.hcount = (uint16_t)v; return 1;
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
        /* The sample FIFOs — see src/sound.c, which owns the unit itself. Only
         * the slave ever writes them here; the 68000's own path tags its
         * writes for itself, in src/gen68k.c. */
        case 0x34: case 0x36: case 0x38:
            sound_pwm_write(((a & 0xFE) - 0x34) / 2, (uint16_t)v,
                            mars_running == &mars_cpu[1] ? 'S' : 'M');
            return 1;
        /* Comm 0, the 68000 rendezvous, is plain storage now: the SH-2 zeroing
         * it is how it tells the 68000 it is ready, and the 68000 waits for
         * exactly that — at 0x8845CE among other places. It used to be
         * intercepted, because commands arrived a whole frame before the SH-2
         * ran and would have been wiped by the zero the dispatch loop writes on
         * entry; the 68000 now drives the SH-2 at the moment it posts, so there
         * is nothing left to paper over. See the merge above. */
        default:
            return 1;
        }
    }
    if (a >= 0x4100 && a < 0x4200) {          /* VDP registers */
        switch (a & 0xFE) {
        case 0x00: mars.bitmap_mode = (uint16_t)v & ~BITMAP_MODE_SET;
                   mars_modes_seen |= (uint8_t)(1u << (v & 3)); return 1;
        case 0x02: mars.shift = (uint16_t)v; return 1;
        /* Eight bits, and the other eight are not there to be written. The
         * fill wraps inside a 256-word block — which is what the address
         * increment in autofill() already models — so a length wider than the
         * block it runs in cannot mean anything. The reference's own clear loop
         * at 0x06003180 says the same from the other side: it writes 0xFF, the
         * maximum, and steps the start address by 0x100, 256 times, to cover
         * the 128 KB buffer exactly.
         *
         * Taking all sixteen bits is what stalled the game after the SEGA logo.
         * The blitter at 0x06004750 hands this a count that can arrive as
         * 0xFFFE, which is 254 words on the machine and 65,534 here — the same
         * 256 words written over and over, but charged 256 times the FEN, and
         * the master waits for every one of them. At 3,600 frames 90% of its
         * cycles were going into fills. */
        case 0x04: mars.fill_len = (uint16_t)v & 0xFFu; return 1;
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
        case 0x04: *out = mars.hcount; return 1;
        case 0x06: *out = mars.dreq_ctl; return 1;
        case 0x10: *out = mars.dreq_len; return 1;
        case 0x30: *out = mars.pwm_ctl; return 1;
        case 0x32: *out = mars.pwm_cycle; return 1;
        /* The sample FIFOs. Bits 15 and 14 are the full and empty flags, and
         * the driver reads the left one between its two writes to decide
         * whether it has room for the second — see src/sound.c. This used to
         * answer a flat zero, "always room", which was right by accident:
         * the unit takes a sample out per cycle where the driver puts two in
         * every second one, so the answer is never anything else. */
        case 0x34: *out = sound_pwm_status(PWM_L); return 1;
        case 0x36: *out = sound_pwm_status(PWM_R); return 1;
        case 0x38: *out = sound_pwm_status(PWM_MONO); return 1;
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
    (void)c; a = canon(a);
    uint32_t v;
    if (a < 0x10000u && mars_reg_read_sh2(a, &v)) return (uint8_t)((a & 1) ? v : v >> 8);
    uint8_t *p = resolve(a, 1);
    if (p) return *p;
    trap("r8", a);
    return 0;
}
uint16_t sh2_r16(SH2 *c, uint32_t a) {
    (void)c; a = canon(a);
    uint32_t v;
    if (a < 0x10000u && mars_reg_read_sh2(a, &v)) return (uint16_t)v;
    uint8_t *p = resolve(a, 2);
    if (p) return (uint16_t)((p[0] << 8) | p[1]);
    trap("r16", a);
    return 0;
}
uint32_t sh2_r32(SH2 *c, uint32_t a) {
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
    (void)c; uint32_t ra = canon(a);
    if (mars_watch_hi) watch("w8", ra, v);
    if (is_purge(ra)) return;
    if (ra < 0x10000u && mars_reg_write_sh2(ra, v, 1)) return;
    if (is_overwrite(ra) && v == 0) return;
    uint8_t *p = resolve(ra, 1);
    if (p) { if (ra < 0x02000000u || ra >= 0x03000000u) *p = v; return; }
    trap("w8", ra);
}
void sh2_w16(SH2 *c, uint32_t a, uint16_t v) {
    (void)c; uint32_t ra = canon(a);
    if (mars_watch_hi) watch("w16", ra, v);
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
    /* Before the split, because starting a division is a whole-longword event:
     * the unit divides when the low half of the dividend lands, and two 16-bit
     * halves would run it on a half-written operand. */
    uint32_t ra = canon(a);
    if (ra >= 0xFFFFFE00u && divu_w32(ra, v)) return;
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
/* Every block, not the first 1,024 of them. The cap silently excluded
 * everything above 0x060039xx — which is the whole of the 3D path, so the one
 * question "does the polygon renderer run at all" could not be asked. */
#define TRACE_MAXFN 2048
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

    /* Hottest functions, which is usually where a runaway loop lives — and,
     * with enough of them, what says which draw path a screen is using. */
#define TRACE_TOP 24
    unsigned top[TRACE_TOP] = {0}, ntop = 0;
    for (unsigned i = 0; i < sh2_function_count && i < TRACE_MAXFN; i++) {
        if (!tcount[i]) continue;
        unsigned j = ntop < TRACE_TOP ? ntop++ : TRACE_TOP;
        while (j > 0 && tcount[top[j - 1]] < tcount[i]) {
            if (j < TRACE_TOP) top[j] = top[j - 1];
            j--;
        }
        if (j < TRACE_TOP) top[j] = i;
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

/* A run of the same block in the same state, not yet written out: what it is a
 * run of, the state, and how many entries have followed the one on the file. */
static uint32_t run_addr[2];
static SH2 run_state[2];
static unsigned long run_n[2];
static int run_open[2];

/* Would the two produce the same trace line? Exactly the fields sh2_trace_line
 * prints, so that "collapsed" and "identical" mean the same thing. */
static int same_state(const SH2 *a, const SH2 *b) {
    for (unsigned k = 0; k < 16; k++)
        if (a->r[k] != b->r[k]) return 0;
    return a->pr == b->pr && a->gbr == b->gbr && a->vbr == b->vbr
        && a->mach == b->mach && a->macl == b->macl
        && a->t == b->t && a->m == b->m && a->q == b->q
        && a->s == b->s && a->imask == b->imask;
}

int sh2_trace_open(const char *path, unsigned long max_lines) {
    sh2_trace_f = fopen(path, "w");
    if (!sh2_trace_f) { perror(path); return 0; }
    sh2_trace_left[0] = sh2_trace_left[1] = max_lines;
    run_open[0] = run_open[1] = 0;
    sh2_trace = 1;
    return 1;
}

static void sh2_trace_line(const SH2 *c, uint32_t addr) {
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

/* Close an open run with the count of what was left out.
 *
 * This is the reference tracer's own shape, and matching it exactly is the
 * point: a printed line, then `[Omitted: N]`, then the next *different* line.
 * Writing a trailing copy of the block as well seemed harmless and was not —
 * it gives our stream a repeat the reference's format never produces, and the
 * slave's alignment fell from 269 blocks to 196 on it.
 */
static void sh2_trace_flush(int i) {
    if (!run_open[i]) return;
    run_open[i] = 0;
    if (run_n[i] && sh2_trace_left[i]) {
        sh2_trace_left[i]--;
        fprintf(sh2_trace_f, "%s Instruction: [Omitted: %lu]\n",
                i ? "SHS" : "SHM", run_n[i]);
    }
    run_n[i] = 0;
}

void sh2_trace_close(void) {
    if (sh2_trace_f) {
        sh2_trace_flush(0);
        sh2_trace_flush(1);
        fclose(sh2_trace_f);
    }
    sh2_trace_f = NULL;
    sh2_trace = 0;
}

/* One line per basic block entered — except that a CPU with nothing to do
 * re-enters one block millions of times, and writing every one of those buries
 * the run in its own idling. 2.67 million of the slave's first three million
 * lines were the `dt`/`bf` delay loop it waits in between PWM interrupts.
 *
 * A run of the same block *in the same state* therefore keeps its first entry
 * and a count. Nothing is lost by construction — every line left out would have
 * been a copy of the one written — and `[Omitted: N]` is the reference tracer's
 * own format for it, which `tools/tracediff.py` already parses.
 *
 * Collapsing on the address alone is the reference's own policy — it prints one
 * line of the slave's `dt`/`bf` delay loop and omits 197, so the belief recorded
 * here before, that it logs every iteration of a loop whose registers move, was
 * simply wrong. Matching it is still not worth it, and that has now been
 * measured twice. It buys the slave more of the extract per line of trace, and
 * costs the master a quarter of its agreement: 193 blocks in lock step to 143,
 * because the master's poll and its work interleave at the same address in
 * different states and folding them together loses the distinction the
 * comparison is made of.
 */
void sh2_block(SH2 *c, uint32_t addr) {
    if (!sh2_trace_f || !trace_armed) return;
    int i = c->slave & 1;
    if (run_open[i] && addr == run_addr[i] && same_state(c, &run_state[i])) {
        run_n[i]++;
        return;
    }
    sh2_trace_flush(i);
    if (!sh2_trace_left[i]) return;
    sh2_trace_left[i]--;
    sh2_trace_line(c, addr);
    run_addr[i] = addr;
    run_state[i] = *c;
    run_open[i] = 1;
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

int32_t sh2_fuel;

int sh2_survive_missing;
#define MISSING_MAX 64
static uint32_t missing_at[MISSING_MAX];
static unsigned missing_n;

void sh2_call(SH2 *c, uint32_t addr) {
    int recovering = 0;
    /* Flat: calls, returns and jumps are all just a new address. Nothing
     * recurses, so a handler that branches away instead of returning - which
     * is ordinary SH-2 practice - costs nothing here either. */
    SH2 *outer = mars_running;
    mars_running = c;
    while (addr && addr != SH2_YIELD) {
        if (addr < 0x40000000u) addr &= 0x1FFFFFFFu;
        int i = lookup(addr);
        if (i < 0) {
            /* Each of these has been one play session to find, because the
             * first one parks the CPU and ends the run. Distinct addresses are
             * reported rather than the first sixteen occurrences of one, and
             * `--survive-missing` carries on at PR instead of parking: the
             * handler's work is skipped and the game is wrong from that point,
             * but a single session surfaces several addresses instead of one.
             * Two in a row with nothing between them is a loop, so that parks. */
            int seen = 0;
            for (unsigned k = 0; k < missing_n; k++)
                if (missing_at[k] == addr) { seen = 1; break; }
            if (!seen && missing_n < MISSING_MAX) {
                missing_at[missing_n++] = addr;
                fprintf(stderr, "  [call] no recompiled block at 0x%08X\n", addr);
            }
            mars.missing++;
            if (sh2_survive_missing && c->pr && !recovering) {
                recovering = 1;
                addr = c->pr;
                continue;
            }
            addr = 0;
            break;
        }
        recovering = 0;
        if (mars.trace) {
            trace_enter(addr, 0, 0);
            if (i < TRACE_MAXFN) tcount[i]++;
        }
        trap_fn = addr;
        addr = sh2_functions[i].fn(c, addr);
    }
    /* A yield has already parked the resume address; anything else ends the
     * run, and 0 is what says "this CPU has nothing to go back to". */
    if (addr != SH2_YIELD) c->pc = addr;
    mars_running = outer;
}

/* One slice. The CPU picks up wherever the last one left it, which for a CPU
 * parked on a poll is the poll itself — so an idle SH-2 now spends its slice
 * spinning exactly as the hardware does, instead of being unwound and skipped.
 */
unsigned sh2_run(SH2 *c, int32_t fuel) {
    if (!c->pc) return 0;
    sh2_fuel = fuel;
    sh2_call(c, c->pc);
    return (unsigned)(fuel - sh2_fuel);
}

/* Take an external interrupt: redirect the CPU at its handler and return.
 *
 * The SH-2 takes external interrupts through auto-vectors 64-71, two levels to
 * a vector, so the command interrupt at level 8 lands on vector 68. Both of
 * this cartridge's tables fill all eight with one dispatcher — 0x060001B0 on
 * the master, 0x060001F8 on the slave — which reads the accepted level back out
 * of SR to pick the real handler, so the mask has to be set before entry.
 *
 * SR and PC go on the SH-2's own stack, and `rte` pops them: that is what makes
 * the interrupt resumable, and it is what the hardware does. The reference
 * shows both halves — the slave's r15 goes 0xC0000800 -> 0xC00007F8 as it takes
 * the PWM interrupt with its mask going 2 -> 6, and the rte at 0x06000216 puts
 * both back. This used to run the handler to completion with nothing pushed,
 * which worked only because no other CPU could be waiting mid-instruction.
 *
 * A masked interrupt is declined, which the old model had no way to express.
 */
void mars_deliver_int(int slave, unsigned level) {
    SH2 *c = &mars_cpu[slave & 1];
    if (level <= c->imask) {
        if (level == MARS_INT_V) mars.vints_declined[slave & 1]++;
        return;
    }
    if (level == MARS_INT_V) mars.vints[slave & 1]++;
    uint32_t vec = c->vbr + 4u * (64u + ((level + 1u) >> 1));
    const uint8_t *p = resolve(canon(vec), 4);
    if (!p) { trap("vector", vec); return; }
    uint32_t handler = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                     | ((uint32_t)p[2] << 8) | p[3];
    if (!handler || !c->pc) return;

    SH2 *outer = mars_running;
    mars_running = c;
    c->r[15] -= 4; sh2_w32(c, c->r[15], sh2_get_sr(c));
    c->r[15] -= 4; sh2_w32(c, c->r[15], c->pc);
    mars_running = outer;
    c->imask = level & 0xF;
    c->pc = handler;
}

/* --- an interrupt raised by the other CPU, and when it may be acted on -----
 *
 * The three CPUs take turns inside one hand-over: the 68000 runs its whole
 * share of the window, then the SH-2s run theirs. So an interrupt the 68000
 * raises at the *end* of its share was, until this, acted on by an SH-2 from
 * the *start* of the same window — an answer that precedes the question by up
 * to a whole hand-over.
 *
 * The game measures that directly, and the count is in the trace already. The
 * 68000 arms a DREQ transfer at 0x883232, raises the master's command interrupt,
 * and spins at 0x88323A on the bit until the master's handler clears it —
 * eighteen instructions away, twelve of the dispatcher at 0x060001B0 and six
 * more to the `mov.w r0,@r0` at 0x0600133E. So how many times the 68000 goes
 * round says how long the master took: the reference goes round twice in 45 of
 * its 103 transfers, three times in 26 and once in 31, mean 1.97. Ours went
 * round once in 194 of 270, mean 1.28, because whenever the write landed near
 * the end of a hand-over the master answered before the 68000 executed another
 * instruction at all. With the split it is 1.57 — what is left is that our
 * master reaches that ack in 29 SH-2 cycles where the reference's poll counts
 * price it at about 135, which is `sh2_cpi1000` being a frame average applied
 * to eighteen instructions of nothing but memory access.
 *
 * So the raise carries *where in the window it happened*, in the target's own
 * cycles, and the target's slice is split there. One slot per CPU is enough:
 * a second raise while the first is still pending is the same asserted line,
 * and the earlier offset is the one that matters.
 */
typedef struct { unsigned at, level; int armed; } Pending;
static Pending pend[2];

void mars_raise_int(int slave, unsigned level, unsigned at) {
    Pending *p = &pend[slave & 1];
    if (p->armed && p->at <= at) return;
    p->at = at;
    p->level = level;
    p->armed = 1;
}

unsigned mars_int_due(int slave, unsigned cycles) {
    const Pending *p = &pend[slave & 1];
    return p->armed && p->at < cycles ? p->at : cycles;
}

void mars_int_fire(int slave) {
    Pending *p = &pend[slave & 1];
    if (!p->armed) return;
    p->armed = 0;
    mars_deliver_int(slave, p->level);
}

/* A raise can fall past the end of the hand-over it was made in — the 68000
 * wrote in its last cycles, or a recompiled block was charged whole and put
 * every access inside it at the block's end. Carrying the remainder into the
 * next hand-over is what keeps that an offset rather than a clamp: the event is
 * still in the future, just less of it. Called once a hand-over is over, with
 * how much of the target's clock it was worth. */
void mars_int_rebase(unsigned elapsed) {
    for (int i = 0; i < 2; i++)
        pend[i].at = pend[i].at > elapsed ? pend[i].at - elapsed : 0;
}

/* The PWM timer, which is the only clock the slave has.
 *
 * Its sound driver programs the two registers from 0xC0000008 and then idles in
 * a delay loop; every PWM interrupt is one sample period, and nothing else ever
 * wakes it. So the rate is the machine's own rather than a number picked to
 * look right: the SH-2 clock divided by the programmed cycle length, divided
 * again by TM, the interrupt-every-N-cycles field.
 *
 * This game writes cycle 0x417 and TM 1. Read as `cycle - 1` that is 1,046 SH-2
 * cycles, 21,999.4 Hz — the round 22 kHz a sound driver would be aiming at, to
 * three parts in a hundred thousand — and 366.65 interrupts to a 60 Hz frame.
 */
#define SH2_CLOCK 23011360u              /* NTSC 32X */

/* The period itself, in SH-2 cycles, rather than a count per frame.
 *
 * A count per frame has to be an integer, and this one is 366.65 — so it was
 * 365, losing an interrupt every three frames, and the frame's worth of them was
 * then spread over the 262 scanlines, which put each one at a line boundary up
 * to seven hundred instructions from where the timer would have fired it. The
 * period divides no more than once and the frame loop counts SH-2 cycles
 * against it, so neither error is left.
 *
 * `cycle - 1`, and it was `cycle + 1`, which is two cycles in 1,047 — 0.19% on
 * the only clock the slave has. Measured rather than read off a manual, and
 * without needing a clock to measure it with: a loop that closes on itself
 * costs exactly what its instructions cost, so its laps are a clock, and
 * counting PWM interrupts against them says what one is worth.
 * `tools/refpoll.py --period` does that over the reference, and three loops of
 * quite different shapes — a two-instruction poll of work RAM, a five
 * instruction long-word fill ending in `dbf`, and a two-instruction poll of a
 * 32X register — put it at 1,045.7, 1,045.1 and 1,046.6 SH-2 cycles against the
 * 1,048 this returned. The sample rate is the other confirmation: 1,046 divides
 * the SH-2 clock to 21,999.4 Hz where 1,048 gives 21,956, and a driver aims at
 * the round one.
 *
 * It is two periods, not one. The unit takes a sample out of each FIFO every
 * PWM cycle and raises the interrupt every TM of them, so the sample clock
 * below is the one src/sound.c counts against and the interrupt is TM times it.
 * They coincide here only because this game writes TM 1. */
unsigned mars_pwm_sample_period(void) {
    unsigned cycle = mars.pwm_cycle & 0xFFF;
    return cycle < 2 ? 0 : cycle - 1;
}

/* The timer interrupt's period, which is TM of those. TM zero switches the
 * interrupt off without stopping the sample clock. */
unsigned mars_pwm_period(void) {
    unsigned tm = (mars.pwm_ctl >> 8) & 0xF;
    unsigned p = mars_pwm_sample_period();
    return tm ? p * tm : 0;
}

/* The sample rate the two registers come out at: 22,000 Hz here. */
unsigned mars_pwm_rate(void) {
    unsigned p = mars_pwm_sample_period();
    return p ? SH2_CLOCK / p : 0;
}

/* Only the end-of-run report wants this shape. */
unsigned mars_pwm_per_frame(void) {
    unsigned p = mars_pwm_period();
    return p ? SH2_CLOCK / p / 60 : 0;
}

/* The adapter's boot ROM hands the cartridge its two SH-2s.
 *
 * The reference puts this after the 68000 has been running a while: the master
 * enters 0x060001A0 379,013 log lines past the reset and the slave 0x060001A4
 * at 379,065, by which point the 68000 has passed both the checksum rendezvous
 * and the M_OK / S_OK one. Holding them until then is what keeps the master's
 * dispatch loop from reading the handshake words as if they were a command —
 * its loop head writes 0 to comm 0 before reading, so once it is running the
 * register is its own.
 */
void mars_bios_handover(void) {
    static int done;
    if (done) return;
    done = 1;
    mars_cpu[0].pc = MARS_MASTER_ENTRY;
    mars_cpu[1].pc = MARS_SLAVE_ENTRY;
}

/* A command used to be run here and then, from inside the 68000's register
 * write, because the master was only reachable that way. It is running on its
 * own slice now: it is sitting in its dispatch poll, it will read the word the
 * 68000 has just stored on its next turn, and the 68000 spins meanwhile exactly
 * as it does on hardware. Nothing to do but count it. */
void mars_run_command(void) {
}

void sh2_unimplemented(SH2 *c, uint32_t addr, const char *what) {
    (void)c;
    fprintf(stderr, "  [sh2] unimplemented at 0x%08X: %s\n", addr, what);
}

