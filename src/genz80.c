/* The Z80's side of the Mega Drive: its 8 KB, its window into the 68000's
 * address space, and the arbitration between the two.
 *
 * Nothing here existed until now — src/gen68k.c discarded every write to
 * 0xA00000 and every write to the bus and reset registers, so the driver the
 * 68000 uploads went nowhere and the Z80 had nothing to run. What the reference
 * shows at the machine's reset is two programs, not one: a short stub the
 * cartridge leaves in place first, `di / im 1 / jp 0x005B`, and then, after the
 * 68000 has loaded the real driver and pulsed reset, a restart at 0x0000 with
 * `xor a / ld bc,0x1FD9` — the SMPS clear-and-go. Getting the upload wrong is
 * therefore visible on the second instruction, which is what makes this
 * checkable at all.
 *
 * The Z80's map:
 *
 *   0x0000-0x1FFF  its RAM, mirrored through 0x3FFF
 *   0x4000-0x5FFF  the YM2612, four registers mirrored every four bytes
 *   0x6000         the bank register: one bit a write, nine writes to a bank
 *   0x7F00-0x7FFF  the Mega Drive VDP
 *   0x8000-0xFFFF  a 32 KB window on the 68000's space at bank << 15
 *
 * And the 68000's side of it: 0xA00000 is the same 8 KB, 0xA11100 asks for the
 * bus and 0xA11200 holds reset. The Z80 only runs when it has the bus and is
 * out of reset, which is the whole of the arbitration this machine needs — the
 * 68000 never reads Z80 RAM back, and a real bus conflict would need both to be
 * running at once, which by construction they are not.
 */
#include <stdio.h>
#include <string.h>
#include "mars.h"
#include "sound.h"
#include "genz80.h"
#include "m68k.h"

Z80 z80;
uint8_t z80_ram[8 * 1024];

/* Held by the 68000, and cleared by it. Both start asserted: a Mega Drive comes
 * up with the Z80 in reset and the 68000 owning the bus. */
static int z80_busreq = 1, z80_reset_held = 1;
static unsigned z80_bank;            /* the nine bits, already shifted */

/* What the 68000 did to it, for the report — a run where the driver never
 * arrives looks exactly like one where it arrives and does nothing. */
uint32_t z80_bytes_written, z80_resets, z80_bank_writes;

int genz80_runnable(void) { return !z80_busreq && !z80_reset_held; }

/* ---------------------------------------------------------- the Z80's bus -- */
uint8_t z80_rd(uint16_t a) {
    if (a < 0x4000) return z80_ram[a & 0x1FFF];
    /* The YM2612's status byte: bit 7 busy, bits 1-0 the timer flags. Zero is
     * "idle, nothing expired", which is the answer that cannot hang a driver
     * waiting on it — and neither the Z80 nor the 68000 reads it in this game,
     * so it is a stand-in rather than a model. */
    if (a < 0x6000) return 0x00;
    if (a < 0x8000) return 0xFF;
    return (uint8_t)m68k_read_memory_8(((z80_bank << 15) | (a & 0x7FFF))
                                       & 0xFFFFFFu);
}

void z80_wr(uint16_t a, uint8_t v) {
    if (a < 0x4000) { z80_ram[a & 0x1FFF] = v; return; }
    if (a < 0x6000) { sound_ym_write(a & 3, v); return; }
    if (a < 0x6100) {
        /* One bit a write, shifted in at the top, so nine writes make a bank.
         * The reference's driver does this 2,498 times over the extract, which
         * is how it reaches its sample data in the cartridge. */
        z80_bank = ((z80_bank >> 1) | ((unsigned)(v & 1) << 8)) & 0x1FF;
        z80_bank_writes++;
        return;
    }
    if (a < 0x8000) {
        /* The VDP, as the Z80 sees it. 0x7F11 is the PSG. */
        if ((a & 0xFF) == 0x11) { sound_psg_write(v); return; }
        m68k_write_memory_8(0xC00000u | (a & 0x1F), v);
        return;
    }
    m68k_write_memory_8(((z80_bank << 15) | (a & 0x7FFF)) & 0xFFFFFFu, v);
}

/* The Z80 on this machine has nothing on its I/O ports; everything is memory
 * mapped. Modelled so that an `in`/`out` shows up as a finding rather than as
 * a crash. */
static uint32_t z80_io_seen;

uint8_t z80_io_rd(uint16_t port) {
    if (z80_io_seen++ < 4)
        fprintf(stderr, "  [z80] in from port 0x%04X at 0x%04X\n", port, z80.pc);
    return 0xFF;
}

void z80_io_wr(uint16_t port, uint8_t v) {
    if (z80_io_seen++ < 4)
        fprintf(stderr, "  [z80] out 0x%02X to port 0x%04X at 0x%04X\n",
                v, port, z80.pc);
}

/* ------------------------------------------------------- the 68000's side -- */
int genz80_read(uint32_t a, uint8_t *out) {
    if (a >= 0xA00000u && a < 0xA04000u) { *out = z80_ram[a & 0x1FFF]; return 1; }
    if (a >= 0xA04000u && a < 0xA06000u) { *out = 0x00; return 1; }  /* YM status */
    if ((a & ~1u) == 0xA11100u) {
        /* Bit 8 reads back set while the Z80 still has the bus. We grant it
         * immediately, so a request that has been made always reads granted —
         * which is what the reference's 68000 sees too: it polls this seven
         * times in the boot and goes on. */
        *out = (a & 1) ? 0x00 : (uint8_t)(z80_busreq ? 0x00 : 0x01);
        return 1;
    }
    if ((a & ~1u) == 0xA11200u) { *out = 0; return 1; }
    return 0;
}

int genz80_write(uint32_t a, uint8_t v) {
    if (a >= 0xA00000u && a < 0xA04000u) {
        z80_ram[a & 0x1FFF] = v;
        z80_bytes_written++;
        return 1;
    }
    if (a >= 0xA04000u && a < 0xA06000u) { sound_ym_write(a & 3, v); return 1; }
    if (a >= 0xA06000u && a < 0xA06100u) {
        z80_bank = ((z80_bank >> 1) | ((unsigned)(v & 1) << 8)) & 0x1FF;
        return 1;
    }
    if (a >= 0xA00000u && a < 0xA10000u) return 1;       /* the rest of its space */
    if ((a & ~1u) == 0xA11100u) {
        if (!(a & 1)) z80_busreq = (v & 1) != 0;
        return 1;
    }
    if ((a & ~1u) == 0xA11200u) {
        if (!(a & 1)) {
            int held = !(v & 1);
            /* The falling edge is the reset; the Z80 restarts when it is let
             * go. Counting them is what says the driver was actually started
             * rather than merely copied. */
            if (!held && z80_reset_held) { z80_reset(&z80); z80_resets++; }
            z80_reset_held = held;
        }
        return 1;
    }
    return 0;
}

/* Word accesses reach an 8-bit bus, so both halves see the high byte — which is
 * what a 68000 writing `move.w` into Z80 RAM actually leaves behind. */
int genz80_read16(uint32_t a, uint16_t *out) {
    uint8_t b;
    if (!genz80_read(a, &b)) return 0;
    *out = (uint16_t)((b << 8) | b);
    return 1;
}

int genz80_write16(uint32_t a, uint16_t v) {
    return genz80_write(a, (uint8_t)(v >> 8));
}

void genz80_init(void) {
    z80_reset(&z80);
    memset(z80_ram, 0, sizeof z80_ram);
}

/* One slice, if it is the Z80's to run. */
unsigned genz80_slice(int cycles) {
    if (!genz80_runnable()) return (unsigned)cycles;  /* held: the time is spent */
    return z80_run(&z80, cycles);
}

/* ----------------------------------------------------------- the tracer ----
 *
 * The same shape as src/trace68k.c: the reference tracer's fields, the register
 * state *before* the instruction, and a run of identical lines collapsed into
 * one "[Omitted: N]" the way the reference collapses its own. The mnemonic
 * column carries the opcode byte instead, because tools/diffz80.py reads the
 * `name:value` fields and annotates addresses itself.
 */
static FILE *tf;
static unsigned long emitted, limit;
static Z80 run_state;
static unsigned long run_n;
static int run_open;

static int same_state(const Z80 *a, const Z80 *b) {
    return a->pc == b->pc && a->a == b->a && a->f == b->f
        && a->b == b->b && a->c == b->c && a->d == b->d && a->e == b->e
        && a->h == b->h && a->l == b->l
        && a->ix == b->ix && a->iy == b->iy && a->sp == b->sp
        && a->iff1 == b->iff1 && a->iff2 == b->iff2 && a->im == b->im;
}

static void flush_run(void) {
    if (!run_open) return;
    run_open = 0;
    if (run_n && (!limit || emitted < limit)) {
        emitted++;
        fprintf(tf, "APU Instruction: [Omitted: %lu]\n", run_n);
    }
    run_n = 0;
}

void z80_trace_line(const Z80 *c) {
    if (!tf) return;
    if (run_open && same_state(c, &run_state)) { run_n++; return; }
    flush_run();
    if (limit && emitted >= limit) return;
    emitted++;
    run_state = *c;
    run_open = 1;
    fprintf(tf, "APU  %04x  %02x  AF:%02x%02x BC:%02x%02x DE:%02x%02x "
                "HL:%02x%02x IX:%04x IY:%04x SP:%04x IFF:%d%d IM:%d\n",
            c->pc, z80_ram[c->pc & 0x1FFF], c->a, c->f, c->b, c->c, c->d, c->e,
            c->h, c->l, c->ix, c->iy, c->sp, c->iff1 ? 1 : 0, c->iff2 ? 1 : 0,
            c->im);
}

int z80_trace_open(const char *path, unsigned long max_lines) {
    tf = fopen(path, "w");
    if (!tf) { perror(path); return 0; }
    limit = max_lines;
    z80_tracing = 1;
    return 1;
}

void z80_trace_close(void) {
    if (!tf) return;
    flush_run();
    fclose(tf);
    tf = NULL;
    z80_tracing = 0;
    printf("  Z80 trace: %lu instruction(s) written%s\n", emitted,
           limit && emitted >= limit ? " (line limit reached)" : "");
}
