/* The Mega Drive half of the machine: the 68000's address space.
 *
 * The 68000 is the engine in this game — it decides what to draw and posts
 * commands to the SH-2s. The two CPUs meet only at the 32X communication
 * registers, which appear here at 0xA15120 and on the SH-2 side at 0x4020, so
 * both views index the same `mars.comm[]`.
 *
 * The Genesis VDP lives here as state: the control port's address/code latch,
 * the data port, DMA from 68000 memory, and a status register whose blanking
 * bits advance so that polling loops terminate. Turning that state into pixels
 * is src/genvdp.c, and in this game that is where the picture comes from — the
 * 32X frame buffer is composited over it but holds no image yet.
 */
#include <stdio.h>
#include <string.h>
#include "mars.h"
#include "sound.h"
#include "genz80.h"
#include "m68k.h"

Gen gen;

#define IN(a, lo, hi) ((a) >= (lo) && (a) < (hi))

/* The adapter's 68000-side vector RAM: 256 bytes at 0x000000 that the 32X
 * supplies in place of the cartridge, holding the exception vectors plus a
 * couple of BIOS helper routines tucked into the slots the 68000 leaves
 * reserved. The reference trace shows all three of its distinguishing
 * behaviours: the cartridge writes vector 28 at 0x000070 (from 0x88077E and
 * again at 0x8809EA), which only works if the region takes writes; it calls a
 * routine at 0x0000C0 (from 0x8809E0) whose first opcode is 0x08F9, where the
 * cartridge's own image holds the 0x00880B2E handler pointer that fills every
 * reserved slot; and everything from 0x000100 up matches the cartridge byte for
 * byte, which is how the security stub at 0x0003F0 ran in lock step.
 *
 * We have no BIOS image, so the vectors are seeded from the cartridge's own
 * table — the handlers the game expects to reach — and the one helper the
 * cartridge calls is assembled from the four instructions the reference
 * executes there:
 *
 *      0000c0  08f9 0000 00a15107   bset.b  #0,($a15107)    ; RV on
 *      0000c8  1280                 move.b  d0,(a1)         ; a1 = 0xA130F1
 *      0000ca  08b9 0000 00a15107   bclr.b  #0,($a15107)    ; RV off
 *      0000d2  4e75                 rts
 *
 * Flipping RV around the write is what lets the 68000 reach the cartridge's own
 * mapper register at 0xA130F1. Both of those registers are already no-ops on
 * this side, so the routine's only effect that matters here is that it returns.
 */
static const uint8_t bios_helper_c0[] = {
    0x08, 0xF9, 0x00, 0x00, 0x00, 0xA1, 0x51, 0x07,
    0x12, 0x80,
    0x08, 0xB9, 0x00, 0x00, 0x00, 0xA1, 0x51, 0x07,
    0x4E, 0x75,
};

/* The adapter's table names a stub, where the cartridge's own names the stub's
 * target. The cartridge shows this itself: at 0x88077E it writes 0x008802A2
 * into vector 28, and 0x8802A2 is `jmp 0xffffc036` — which is exactly the value
 * its own header holds for that vector. Vector 30, the vertical interrupt, is
 * the same shape: the header says 0xFFFFC030 and 0x8802AE is `jmp 0xffffc030`.
 * The reference executes 0x8802AE on every one of its 102 vblanks, so that is
 * what the adapter has there, and seeding the header value skipped one
 * instruction each time. */
static const struct { uint8_t vec; uint32_t at; } vector_stubs[] = {
    { 28, 0x008802A2 }, { 30, 0x008802AE },
};

void gen68k_init_vectors(void) {
    memcpy(gen.vecram, mars.rom, sizeof gen.vecram);
    memcpy(&gen.vecram[0xC0], bios_helper_c0, sizeof bios_helper_c0);
    for (unsigned i = 0; i < sizeof vector_stubs / sizeof *vector_stubs; i++) {
        uint8_t *p = &gen.vecram[vector_stubs[i].vec * 4];
        uint32_t v = vector_stubs[i].at;
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
    }
}

/* --- what the 32X BIOS puts in the comm registers --------------------------
 *
 * We have no BIOS image and start the SH-2s at the cartridge entry points, so
 * these have to be supplied; the reference shows all of them written by SH-2
 * code below 0x00000300, which is the adapter's boot ROM rather than the
 * cartridge.
 *
 *   comm 4    the cartridge checksum, summed by the master at 0x00000278 and
 *             posted at 0x00000284. The 68000 waits for it at 0x8807C2 and then
 *             compares it against the header's own word at 0x88018E.
 *   comm 0-3  "M_OK" and "S_OK", written by the slave around 0x000001C0. The
 *             68000 waits for those at 0x8809A6 and 0x8809B2.
 *
 * None of it can be posted before the 68000 starts, because the 68000 zeroes
 * these registers itself as part of its own boot — comm 4 at 0x0003F6, comm 0-3
 * at 0x8806F0 — and only then waits for the BIOS to fill them. So each value is
 * delivered on the 68000's own next read after it has done that clearing, which
 * from its side is indistinguishable from the BIOS having got there first.
 *
 * Delivering at the read rather than once a frame is what makes it survive the
 * slave: its sound driver uses comm 4 for its own traffic, and once the PWM
 * timer runs it writes there 366 times a frame. On hardware the ordering is the
 * same and never collides — the 68000 reaches 0x8807C2 at reference line
 * 378,847, the slave enters cartridge code at 379,065, and its first PWM
 * interrupt is at 379,271, by which point the handshake is long done.
 */
uint16_t mars_rom_checksum(void) {
    uint32_t sum = 0;
    for (uint32_t o = 0x200; o + 1 < mars.rom_size; o += 2)
        sum += ((uint16_t)mars.rom[o] << 8) | mars.rom[o + 1];
    return (uint16_t)sum;
}

static struct { int cleared_sum, cleared_ok, sum_done, ok_done; } bios;

static void bios_comm_write(unsigned i, uint16_t v) {
    if (v) return;
    if (i == 4) bios.cleared_sum = 1;
    if (i < 4 && !mars.comm[0] && !mars.comm[1] && !mars.comm[2] && !mars.comm[3])
        bios.cleared_ok = 1;
}

static void bios_comm_read(unsigned i) {
    if (i == 4 && bios.cleared_sum && !bios.sum_done) {
        mars.comm[4] = mars_rom_checksum();
        bios.sum_done = 1;
        /* Summing the cartridge is the last thing the boot ROM does with the
         * master, so this is where it lets the cartridge have it. The reference
         * puts the two 166 log lines apart: the 68000 reaches the checksum
         * rendezvous at 0x8807C2 at line 378,847 and the master enters
         * 0x060001A0 at 379,013 — before the M_OK check at 0x8809A6, not after.
         *
         * Handing over on the handshake instead is a whole rendezvous too late:
         * the master then arrives at its wait on comm 0-1 after the 68000 has
         * zeroed it and posted a command, and waits for a zero that has already
         * been and gone. Arriving early costs nothing, because that wait is
         * exactly what the reference spends 2,915 instructions doing. */
        mars_bios_handover();
    }
    if (i < 4 && bios.cleared_ok && !bios.ok_done) {
        mars.comm[0] = 0x4D5F; mars.comm[1] = 0x4F4B;   /* M_OK */
        mars.comm[2] = 0x535F; mars.comm[3] = 0x4F4B;   /* S_OK */
        bios.ok_done = 1;
    }
}

/* --- the controller ports --------------------------------------------------
 *
 * Three ports, each a data register with a control register six bytes above it.
 * A control bit of 1 makes that data bit an output, so a read gives back what
 * was written for the outputs and the pad for the inputs. TH — bit 6 — is the
 * only line the game drives, and it selects which half of a three-button pad
 * answers. Everything is active low:
 *
 *     TH high    ? ? C B R L D U
 *     TH low     ? ? S A 0 0 D U
 *
 * The two hard zeroes in the low half are the signature. The identification
 * routine at 0x8F45F0 reads the low nibble with TH high, then again with TH
 * low, and maps the pair through the table at 0x8F4620: a pad answers 0xF then
 * 0x3 and comes out as index 12, which is the `jsr (pc,d0.w)` target 0x8F45E8
 * the reference takes at 0x8F45CA. An empty port floats high both times and
 * gives 14 instead, which is the branch we were taking by having no ports at
 * all — 74 instructions of pad handling skipped, 204 times over the extract.
 *
 * And it is a *six*-button pad. The routine at 0x8F46EE pulses TH three more
 * times after the identification and branches when the low nibble comes back
 * zero, which is the six-button signature and which a three-button pad never
 * produces. The reference takes that branch, so that is what was plugged into
 * the machine the log came from.
 *
 * So the pad answers a rotating sequence keyed to how many times TH has fallen
 * since it was last left alone:
 *
 *     TH high, any cycle    ? ? C B R L D U      (cycle 3 is ? ? C B M X Y Z)
 *     TH low, cycle 3       ? ? S A 0 0 0 0      the signature
 *     TH low, cycle 4       ? ? S A 1 1 1 1
 *     TH low, otherwise     ? ? S A 0 0 D U
 *
 * The real pad forgets the count after about 1.5 ms of TH left high, which is
 * shorter than a frame and longer than one read sequence, so clearing it once a
 * frame is the same thing from the game's side.
 *
 * Only port one carries what the keyboard is holding; the other two answer as
 * pads with nothing pressed, which is what identified all three before there was
 * any input at all and is what keeps that part unchanged.
 */
#define PAD_PORTS 3

static uint8_t pad_lines(unsigned port, int th) {
    /* The six lines, most significant first. A zero entry is a line the pad
     * holds low whatever is pressed — the two that make the three-button half
     * recognisable. */
    static const unsigned high[6]  = { PAD_C, PAD_B, PAD_R, PAD_L, PAD_D, PAD_U };
    static const unsigned extra[6] = { PAD_C, PAD_B, PAD_M, PAD_X, PAD_Y, PAD_Z };
    static const unsigned low[6]   = { PAD_S, PAD_A, 0,     0,     PAD_D, PAD_U };

    const unsigned *sel = th ? (gen.pad_cycle[port] == 3 ? extra : high) : low;
    uint8_t drives = 0x3F, forced = 0;
    if (!th && gen.pad_cycle[port] == 3) drives = 0x30;          /* S A 0 0 0 0 */
    if (!th && gen.pad_cycle[port] == 4) drives = 0x30, forced = 0x0F;

    unsigned held = port < sizeof gen.pad_buttons / sizeof *gen.pad_buttons
                  ? gen.pad_buttons[port] : 0;
    uint8_t v = (uint8_t)(0x40 | forced);       /* TH; the caller masks it out */
    for (unsigned i = 0; i < 6; i++) {
        uint8_t line = (uint8_t)(0x20u >> i);
        if ((drives & line) && sel[i] && !(held & sel[i])) v |= line;
    }
    return v;                                   /* active low: held pulls to 0 */
}

static uint16_t pad_read(unsigned port) {
    uint8_t ctrl = (uint8_t)gen.io[4 + port];
    uint8_t out = (uint8_t)gen.io[1 + port];
    uint8_t pad = pad_lines(port, out & 0x40);
    return (uint16_t)((out & ctrl) | (pad & (uint8_t)~ctrl));
}

/* TH falling is what the pad counts. */
static void pad_write(unsigned port, uint16_t v) {
    uint8_t th = (v & 0x40) != 0;
    if (gen.pad_th[port] && !th && gen.pad_cycle[port] < 8)
        gen.pad_cycle[port]++;
    gen.pad_th[port] = th;
}

void gen68k_frame_start(void) {
    for (unsigned p = 0; p < PAD_PORTS; p++) gen.pad_cycle[p] = 0;
}

static uint32_t rom_at(uint32_t a) {
    if (IN(a, 0x880000u, 0x900000u)) return a - 0x880000u;
    if (IN(a, 0x900000u, 0xA00000u))
        return ((uint32_t)(mars.bank & 3) << 20) + (a - 0x900000u);
    if (a < 0x400000u) return a;
    return 0xFFFFFFFFu;
}

/* ------------------------------------------------------------ Genesis VDP */
/* What a DMA costs the 68000, in thousandths of a scanline.
 *
 * It costs it the bus. The VDP takes the transfers in slots it would otherwise
 * give the CPU, so for as long as one runs the 68000 does not execute — and the
 * rate is in the VDP's own table, in transfers a scanline, which is why that is
 * the unit handed back rather than cycles. Blanking is where nearly all of it
 * happens and is an order of magnitude faster than active display, because the
 * VDP is not fetching pattern data.
 *
 * This was the last of the 68000's residue and it was worth 264 cycles a frame:
 * the engine uploads its palette by DMA every vblank, 64 words to CRAM, and we
 * did the transfer and charged nothing. What the 68000 did with the time was
 * spin 24 more instructions in the wait loop at 0x8834C0 than the reference —
 * which is the whole of why our frame retired 11,634 instructions against its
 * 11,601, the loop being the cheapest thing in the frame at 11 cycles.
 */
static uint32_t dma_lines1000(uint32_t len) {
    int h40 = (gen.vdpreg[12] & 0x81) != 0;
    int blank = gen.line >= 224 || !(gen.vdpreg[1] & 0x40);
    int slow = (gen.vdp_code & 7) != 1;          /* CRAM and VSRAM, not VRAM */
    unsigned per_line = slow ? (blank ? (h40 ? 102 : 83) : (h40 ? 9 : 8))
                             : (blank ? (h40 ? 205 : 167) : (h40 ? 18 : 16));
    return (uint32_t)((uint64_t)len * 1000u / per_line);
}

static void vdp_dma(void) {
    uint32_t len = ((uint32_t)gen.vdpreg[20] << 8) | gen.vdpreg[19];
    uint32_t src = (((uint32_t)gen.vdpreg[23] & 0x7F) << 17)
                 | ((uint32_t)gen.vdpreg[22] << 9)
                 | ((uint32_t)gen.vdpreg[21] << 1);
    if (len == 0) len = 0x10000;
    if (gen.vdpreg[23] & 0x80) return;      /* fill / copy: not needed yet */
    gen.dma_lines1000 += dma_lines1000(len);
    for (uint32_t i = 0; i < len; i++) {
        uint16_t v = (uint16_t)m68k_read_memory_16(src);
        uint32_t a = gen.vdp_addr & 0xFFFFu;
        if ((gen.vdp_code & 7) == 1) {      /* to VRAM */
            gen.vram[a & 0xFFFEu] = (uint8_t)(v >> 8);
            gen.vram[(a & 0xFFFEu) + 1] = (uint8_t)v;
        } else if ((gen.vdp_code & 7) == 3) {                 /* to CRAM */
            gen.cram[(a >> 1) & 0x3F] = v;
        }
        src += 2;
        gen.vdp_addr += gen.vdpreg[15];
    }
    gen.dma_done++;
}

static void vdp_ctrl(uint16_t v) {
    if (gen.vdp_pending) {
        gen.vdp_addr = (gen.vdp_addr & 0x3FFFu) | ((uint32_t)(v & 3) << 14);
        gen.vdp_code = (uint8_t)((gen.vdp_code & 3) | ((v >> 2) & 0x3C));
        gen.vdp_pending = 0;
        if (gen.vdp_code & 0x20) vdp_dma();
        return;
    }
    if ((v & 0xC000) == 0x8000) {           /* register write */
        gen.vdpreg[(v >> 8) & 0x1F] = (uint8_t)v;
        return;
    }
    gen.vdp_addr = (gen.vdp_addr & 0xC000u) | (v & 0x3FFFu);
    gen.vdp_code = (uint8_t)((gen.vdp_code & 0x3C) | ((v >> 14) & 3));
    gen.vdp_pending = 1;
}

static void vdp_data(uint16_t v) {
    uint32_t a = gen.vdp_addr;
    switch (gen.vdp_code & 7) {
    case 1: gen.vram[a & 0xFFFEu] = (uint8_t)(v >> 8);
            gen.vram[(a & 0xFFFEu) + 1] = (uint8_t)v; break;
    case 3: gen.cram[(a >> 1) & 0x3F] = v; break;
    /* 40 entries; & 0x27 keeps four scattered bits, so entries 16
     * apart aliased onto each other instead of wrapping. */
    case 5: gen.vsram[(a >> 1) % 40] = v; break;
    default: break;
    }
    gen.vdp_addr += gen.vdpreg[15];
    gen.vdp_pending = 0;
}

/* The status register: bit 1 DMA busy, bit 2 HBLANK, bit 3 VBLANK, bit 7
 * vertical interrupt pending, bit 9 FIFO empty — and bits 10-15 driven by
 * nothing at all.
 *
 * The game reads this from three places in the whole extract, and between them
 * they pin every bit that is set:
 *
 *   0x0005B0  d0 = 0x3288   the first read of the run, before a single VDP
 *                           register has been written
 *   0x0005F2  d0 = 0x0A8A   waiting out a DMA, then 0x0A88 when it finishes
 *   0x880AAC  d0 = 0x0A88
 *
 * VBLANK is set in all three, including the first, where the frame loop is on
 * line 0 — because the display is still disabled, and a disabled display is in
 * blanking the whole frame. HBLANK stays on a free-running counter: the frame
 * is only advanced a line at a time, so a bit that could not change inside a
 * line is a bit a loop could wait on forever.
 *
 * The vertical-interrupt flag is set in all three too, and the middle one is
 * the interesting case — it is one read out of 16,555 in the same loop, all of
 * them with the bit still set. So this VDP does not clear it when the status is
 * read; it clears it on the interrupt-acknowledge cycle, which during the boot
 * never comes because the 68000 is masked to level 7 throughout. That is also
 * why it is already set on the very first read: the adapter's boot ROM ran
 * 379,000 instructions before the cartridge got control, and every vblank in
 * there set a flag nothing was ever going to acknowledge.
 *
 * Bits 10-15 are the six the VDP does not drive, where it leaves whatever was
 * last on the bus — for a read into a register, the word the 68000 has already
 * prefetched. The three sites give two different values and both come out
 * exactly right: at 0x0005B0 the next word is 0x303C and the top bits read
 * 0x3000, at the other two it is 0x0800 and they read 0x0800. Musashi does not
 * model the prefetch queue (M68K_EMULATE_PREFETCH is off), but by the time a
 * read callback runs its PC is already past the instruction's extension words,
 * so the word sitting there is the one the queue would be holding.
 */
static uint16_t vdp_status(void) {
    uint16_t s = 0x0200;
    if (gen.line >= 224 || !(gen.vdpreg[1] & 0x40)) s |= 0x0008;
    if ((gen.ticks++ % 20) >= 16) s |= 0x0004;
    if (gen.vint_pending) s |= 0x0080;
    s |= (uint16_t)m68k_read_memory_16(m68k_get_reg(NULL, M68K_REG_PC)) & 0xFC00u;
    return s;
}

/* The acknowledge cycle, which is where the VDP drops the pending flag. With
 * M68K_EMULATE_INT_ACK on, Musashi no longer clears the request itself, so this
 * does it — otherwise the level would still be asserted when the handler's
 * `rte` lowered the mask and it would re-enter immediately. */
int gen68k_int_ack(int level) {
    if (level == 6) gen.vint_pending = gen.vint_irq = 0;
    m68k_set_irq(0);
    return M68K_INT_ACK_AUTOVECTOR;
}

/* ------------------------------------------------------- 32X on the bus --- */
static int mars_reg_read(uint32_t a, uint16_t *out) {
    switch (a & ~1u) {
    case 0xA15100: *out = (uint16_t)(mars.adapter | 0x0080); return 1;  /* ADEN */
    case 0xA15102: *out = mars.intctl; return 1;
    case 0xA15104: *out = mars.bank; return 1;
    /* DREQ control. Bit 2 is 68S, which the 68000 sets to open a transfer and
     * reads back for as long as it is open; bit 7 is FIFO full, which is the
     * VDP's to drive and ours is never full, because the SH-2's DMAC is armed
     * before the first word is pushed and drains each one as it arrives. The
     * reference agrees on both: it writes 0x04 at 0x88322C and the `tst.b` at
     * 0x883250 comes back non-zero and positive, all 103 times, never once
     * taking the `bmi` back to it. Answering 0 here was the difference — 100 of
     * the 554 divergences in the extract, and every one of them this flag. */
    case 0xA15106: *out = mars.dreq_ctl; return 1;
    case 0xA1510A: *out = gen.dreq_ctl; return 1;
    default:
        if (IN(a & ~1u, 0xA15120u, 0xA15130u)) {
            unsigned i = (unsigned)(((a & ~1u) - 0xA15120u) / 2);
            bios_comm_read(i);
            *out = mars.comm[i];
            return 1;
        }
        /* The PWM block, which the 68000 sees at the same offsets in its own
         * copy of the register file. It never touches it in this game — the
         * slave's driver owns the unit — but routing it through the one
         * implementation is what keeps that a fact rather than an assumption. */
        if (IN(a & ~1u, 0xA15130u, 0xA15140u)) {
            uint32_t v; mars_reg_read_sh2(0x4030u + ((a & ~1u) - 0xA15130u), &v);
            *out = (uint16_t)v; return 1;
        }
        /* The 32X VDP and palette, as the 68000 sees them. */
        if (IN(a & ~1u, 0xA15180u, 0xA15190u)) {
            uint32_t v; mars_reg_read_sh2(0x4100u + ((a & ~1u) - 0xA15180u), &v);
            *out = (uint16_t)v; return 1;
        }
        if (IN(a & ~1u, 0xA15200u, 0xA15400u)) {
            uint32_t v; mars_reg_read_sh2(0x4200u + ((a & ~1u) - 0xA15200u), &v);
            *out = (uint16_t)v; return 1;
        }
        if (IN(a & ~1u, 0xA15100u, 0xA15120u)) { *out = 0; return 1; }
        return 0;
    }
}

static int mars_reg_write(uint32_t a, uint16_t v) {
    switch (a & ~1u) {
    case 0xA15100: mars.adapter = v; return 1;
    /* Bits 0 and 1 raise the command interrupt on the master and slave SH-2.
     * The interrupted SH-2 clears the bit from its own handler and the 68000
     * waits for that as an acknowledgement — at 0x8819C6 it spins on bit 0
     * before pushing the command list through the DREQ FIFO.
     *
     * Raising it is all that happens here. The handler used to be run to
     * completion inside this write, so the 68000 never once saw the request
     * pending; it now runs on the SH-2's own slices and the 68000 spins for it,
     * which is what the reference does — and the bit is cleared by the handler
     * writing 0x4000|0x1A, not by us.
     *
     * The raise carries where in the hand-over it happened, because the SH-2s
     * run after the 68000 and stand for the same window: without it a write in
     * the last cycles of a hand-over was answered before the 68000's next
     * instruction. */
    case 0xA15102:
        mars.intctl = v;
        if (v & 1) mars_raise_int(0, MARS_INT_CMD, mars_slice_pos());
        if (v & 2) mars_raise_int(1, MARS_INT_CMD, mars_slice_pos());
        return 1;
    case 0xA15104: mars.bank = v; return 1;
    /* Raising 68S is what arms the transfer, so the word count is taken on that
     * edge — the length register is written first, at 0x883228. Taking it on the
     * level instead would rearm on the BIOS helper's `bset`/`bclr` of RV in the
     * same byte. */
    case 0xA15106:
        if ((v & 4) && !(mars.dreq_ctl & 4)) mars.dreq_left = mars.dreq_len;
        mars.dreq_ctl = v;
        return 1;
    case 0xA1510A: gen.dreq_ctl = v; return 1;
    case 0xA15110: mars.dreq_len = v; return 1;
    case 0xA15112: return mars_reg_write_sh2(0x4012u, v, 2);
    default:
        if (IN(a & ~1u, 0xA15120u, 0xA15130u)) {
            unsigned i = (unsigned)(((a & ~1u) - 0xA15120u) / 2);
            mars.comm[i] = v;
            bios_comm_write(i, v);
            /* The master is sitting in its dispatch poll and will read this on
             * its next slice; the 68000 then waits for it to zero the register
             * back, which is the acknowledgement. Posting used to run the whole
             * command inside this write, so the rendezvous always answered on
             * the 68000's first poll. */
            if (i == 0 && v) {
                gen.cmd_posted++;
                if (v < 16) gen.cmd_hist[v]++;
                mars_run_command();
            }
            return 1;
        }
        /* The engine's own 32X init clears the unit here — control and cycle
         * to zero at 0x880724 and 0x880728, and a zero through the mono port at
         * 0x88072C, which is one word into each FIFO long before the slave's
         * driver exists to fill them. The sample ports go straight to the unit
         * rather than through the SH-2 view, only so that the write is tagged
         * with the CPU that made it: the reference's logs are per-CPU, so a
         * word the 68000 put in the FIFO can appear in no slave trace. */
        if (IN(a & ~1u, 0xA15134u, 0xA1513Au)) {
            sound_pwm_write((unsigned)(((a & ~1u) - 0xA15134u) / 2), v, '6');
            return 1;
        }
        if (IN(a & ~1u, 0xA15130u, 0xA15140u))
            return mars_reg_write_sh2(0x4030u + ((a & ~1u) - 0xA15130u), v, 2);
        if (IN(a & ~1u, 0xA15180u, 0xA15190u))
            return mars_reg_write_sh2(0x4100u + ((a & ~1u) - 0xA15180u), v, 2);
        if (IN(a & ~1u, 0xA15200u, 0xA15400u))
            return mars_reg_write_sh2(0x4200u + ((a & ~1u) - 0xA15200u), v, 2);
        if (IN(a & ~1u, 0xA15100u, 0xA15120u)) return 1;   /* DREQ block */
        return 0;
    }
}

/* ------------------------------------------------------------ callbacks --- */
unsigned int m68k_read_memory_8(unsigned int a) {
    a &= 0xFFFFFFu;
    if (a < sizeof gen.vecram) return gen.vecram[a];
    uint32_t o = rom_at(a);
    if (o != 0xFFFFFFFFu && o < mars.rom_size) return mars.rom[o];
    if (IN(a, 0xFF0000u, 0x1000000u)) return gen.ram[a & 0xFFFFu];
    uint8_t b;
    if (genz80_read(a, &b)) return b;
    return (unsigned)(m68k_read_memory_16(a & ~1u) >> ((a & 1) ? 0 : 8)) & 0xFF;
}

unsigned int m68k_read_memory_16(unsigned int a) {
    a &= 0xFFFFFEu;
    if (a < sizeof gen.vecram)
        return ((unsigned)gen.vecram[a] << 8) | gen.vecram[a + 1];
    uint32_t o = rom_at(a);
    if (o != 0xFFFFFFFFu && o + 1 < mars.rom_size)
        return ((unsigned)mars.rom[o] << 8) | mars.rom[o + 1];
    if (IN(a, 0xFF0000u, 0x1000000u))
        return ((unsigned)gen.ram[a & 0xFFFFu] << 8) | gen.ram[(a & 0xFFFFu) + 1];
    if (IN(a, 0xC00000u, 0xC00010u)) {
        if ((a & 0x1F) < 4) return 0;              /* data port read */
        if ((a & 0x1F) < 8) return vdp_status();
        return (unsigned)((gen.ticks * 3) & 0xFFFF);   /* HV counter */
    }
    /* The 32X identifies itself here; the boot code refuses to run without it. */
    if (a == 0xA130EC) return 0x4D41;      /* "MA" */
    if (a == 0xA130EE) return 0x5253;      /* "RS" */
    uint16_t z;
    if (genz80_read16(a, &z)) return z;
    if (IN(a, 0xA10000u, 0xA10020u)) {
        unsigned i = (a & 0x1F) >> 1;
        /* Version register: domestic, NTSC, no expansion, with the 32X bit. */
        if (i == 0) return 0x00A0;
        if (i >= 1 && i <= PAD_PORTS) return pad_read(i - 1);
        return gen.io[i];
    }
    uint16_t v;
    if (mars_reg_read(a, &v)) return v;
    if (gen.unknown_r++ < 12) fprintf(stderr, "  [68k] read 0x%06X\n", a);
    return 0;
}

unsigned int m68k_read_memory_32(unsigned int a) {
    return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2);
}

void m68k_write_memory_8(unsigned int a, unsigned int v) {
    a &= 0xFFFFFFu;
    if (a < sizeof gen.vecram) { gen.vecram[a] = (uint8_t)v; return; }
    if (IN(a, 0xFF0000u, 0x1000000u)) { gen.ram[a & 0xFFFFu] = (uint8_t)v; return; }
    if (IN(a, 0xA10000u, 0xA10020u)) {
        unsigned i = (a & 0x1F) >> 1;
        gen.io[i] = (uint16_t)v;
        if (i >= 1 && i <= PAD_PORTS) pad_write(i - 1, (uint16_t)v);
        return;
    }
    if (genz80_write(a, (uint8_t)v)) return;            /* Z80 RAM, bus, chips */
    /* The PSG, which the 68000 drives directly as well as through the Z80: its
     * own init writes 0x9F 0xBF 0xDF 0xFF here, one per channel, which is
     * attenuation 15 on all four — silence. */
    if ((a & ~2u) == 0xC00011u) { sound_psg_write((uint8_t)v); return; }
    if (IN(a, 0xA11000u, 0xA11400u)) return;            /* the rest of the bus */
    if (a == 0xA130F1) return;                          /* SRAM control */
    uint16_t cur = (uint16_t)m68k_read_memory_16(a & ~1u);
    uint16_t nv = (a & 1) ? (uint16_t)((cur & 0xFF00) | (v & 0xFF))
                          : (uint16_t)((cur & 0x00FF) | ((v & 0xFF) << 8));
    m68k_write_memory_16(a & ~1u, nv);
}

void m68k_write_memory_16(unsigned int a, unsigned int v) {
    a &= 0xFFFFFEu;
    uint16_t w = (uint16_t)v;
    if (a < sizeof gen.vecram) {
        gen.vecram[a] = (uint8_t)(w >> 8);
        gen.vecram[a + 1] = (uint8_t)w;
        return;
    }
    if (IN(a, 0xFF0000u, 0x1000000u)) {
        gen.ram[a & 0xFFFFu] = (uint8_t)(w >> 8);
        gen.ram[(a & 0xFFFFu) + 1] = (uint8_t)w;
        return;
    }
    if (IN(a, 0xC00000u, 0xC00010u)) {
        if ((a & 0x1F) < 4) vdp_data(w);
        else if ((a & 0x1F) < 8) vdp_ctrl(w);
        return;
    }
    if (IN(a, 0xA10000u, 0xA10020u)) {
        unsigned i = (a & 0x1F) >> 1;
        gen.io[i] = w;
        if (i >= 1 && i <= PAD_PORTS) pad_write(i - 1, w);
        return;
    }
    if (genz80_write16(a, w)) return;
    if ((a & ~2u) == 0xC00010u) { sound_psg_write((uint8_t)w); return; }
    if (IN(a, 0xA11000u, 0xA11400u)) return;            /* the rest of the bus */
    if (mars_reg_write(a, w)) return;
    if (IN(a, 0x840000u, 0x880000u)) {                  /* 32X framebuffer */
        uint32_t o = (a - 0x840000u) & 0x1FFFEu;
        int over = a >= 0x860000u;
        uint8_t *fb = mars_fb_draw();
        if (!over || (w >> 8)) fb[o] = (uint8_t)(w >> 8);
        if (!over || (w & 0xFF)) fb[o + 1] = (uint8_t)w;
        return;
    }
    if (rom_at(a) != 0xFFFFFFFFu) return;               /* writes to ROM ignored */
    if (gen.unknown_w++ < 12) fprintf(stderr, "  [68k] write 0x%06X = %04X\n", a, w);
}

void m68k_write_memory_32(unsigned int a, unsigned int v) {
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v & 0xFFFF);
}
