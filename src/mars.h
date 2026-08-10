/* Machine state for the 32X side: memory, VDP registers, and the comm
 * registers through which the (not yet present) 68000 drives the SH-2s. */
#ifndef MARS_H
#define MARS_H

#include <stdint.h>
#include <setjmp.h>
#include "sh2.h"

#define MARS_ROM_MAX   (4u * 1024 * 1024)
#define MARS_SDRAM     (256u * 1024)
#define MARS_FB        (128u * 1024)   /* one frame; there are two of them */
#define MARS_MAX_CMDS  64

typedef struct {
    uint8_t  rom[MARS_ROM_MAX];
    uint32_t rom_size;
    uint8_t  sdram[MARS_SDRAM];
    uint8_t  fb[2][MARS_FB];
    uint8_t  cache[4096];
    uint8_t  cram[512];
    uint8_t  onchip[512];    /* FRT / WDT / DMAC / SCI, as a plain store */

    /* 32X system registers */
    uint16_t adapter, intctl, bank;
    uint16_t comm[8];

    /* The 68000 -> SH-2 data path. The 68000 sets a word count, raises the
     * SH-2's command interrupt, waits for the handler to arm its DMAC, and then
     * pushes the payload word by word into an 8-word FIFO which the DMAC
     * drains. This is how the master's command *list* arrives; the comm
     * register only carries which kind of command it is. */
    uint16_t dreq_ctl, dreq_len;
    uint16_t dreq_left;      /* words still to push before 68S drops */
    uint16_t fifo[8];
    unsigned fifo_n;
    uint32_t dma_words;      /* words the DMAC has moved, for reporting */

    /* PWM. The slave's sound driver is interrupt-driven and nothing else wakes
     * it, so these two registers are what its clock is made of — see
     * mars_pwm_per_frame(). No audio comes out yet; the sample FIFOs accept
     * writes and report space so the driver never stalls on them. */
    uint16_t pwm_ctl, pwm_cycle;
    uint32_t pwm_ints;       /* delivered, for reporting */

    /* 32X VDP registers */
    uint16_t bitmap_mode, shift, fill_len, fill_start, fill_data, fbctl;

    uint32_t ticks;          /* stands in for elapsed time when polling */
    uint32_t unknown;        /* accesses outside the modelled map */
    uint32_t missing;        /* indirect transfers with no recompiled target */
    uint32_t deep;           /* dispatch recursion that hit the depth bound */
    uint32_t bail[4];        /* unwinds, indexed by MARS_BAIL_* reason */
    int      trace;          /* record every function entry */

    unsigned serviced;       /* commands the master has been run for */
} Mars;

/* The Mega Drive side. */
typedef struct {
    uint8_t  ram[64 * 1024];
    uint8_t  vecram[256];    /* what the adapter supplies at 0x000000 */
    uint8_t  vram[64 * 1024];
    uint16_t cram[64], vsram[40];
    uint8_t  vdpreg[32];
    uint32_t vdp_addr;
    uint8_t  vdp_code, vdp_pending;
    uint16_t io[16];
    uint16_t dreq_ctl;
    uint32_t ticks, dma_done, cmd_posted, unknown_r, unknown_w;
    uint32_t cmd_hist[16];   /* commands posted to comm 0, by kind */
    uint32_t line;           /* scanline the 68000 is running inside */
    uint8_t  vint_pending;   /* VDP status bit 7, until the 68000 acknowledges */
    uint8_t  pad_cycle[3], pad_th[3];   /* six-button pad sequencing */
    unsigned layers;         /* 1 plane B, 2 plane A, 4 sprites */
} Gen;

extern Mars mars;
extern Gen gen;

/* The 32X has two frame buffers and FBCR bit 0 selects between them, so one can
 * be drawn into while the other is on screen. Everything the CPUs reach at
 * 0x24000000 (and the 68000 at 0x840000) goes to the one that is not being
 * displayed; the display scans out the other. Modelling only one conflates the
 * two, which shows up as the display finding a half-written line table. */
static inline uint8_t *mars_fb_draw(void) { return mars.fb[(mars.fbctl & 1) ^ 1]; }
static inline uint8_t *mars_fb_shown(void) { return mars.fb[mars.fbctl & 1]; }

/* Run the master for a command the 68000 has just written into comm 0. */
void mars_run_command(void);

/* The 32X registers are visible from both CPUs — 0x4000/0x4100/0x4200 on the
 * SH-2 side, 0xA15100/0xA15180/0xA15200 on the 68000 side. Both go through
 * these so there is one implementation of the VDP and its status bits. */
int mars_reg_read_sh2(uint32_t a, uint32_t *out);
int mars_reg_write_sh2(uint32_t a, uint32_t v, int size);
void mars_reset_budget(void);
void mars_trace_dump(const char *why);
void mars_trace_reset(void);
void mars_tick_budget(void);

/* The SH-2 polls hardware in loops that only a real machine would break out of.
 * When nothing is left to feed it, unwind rather than spin forever. */
extern jmp_buf mars_bail;
#define MARS_BAIL_IDLE  1
#define MARS_BAIL_BUDGET 2
#define MARS_BAIL_DEPTH  3

/* The two SH-2s live here rather than in main() so that the runtime can enter
 * one from anywhere — specifically from a 68000 register write, which is what
 * raising an interrupt is. */
extern SH2 mars_cpu[2];          /* 0 = master, 1 = slave */

/* Take an external interrupt on one SH-2 and run its handler to completion.
 *
 * Our two CPUs are not interleaved, so there is no instruction boundary to
 * interrupt: the SH-2 is parked in its dispatch loop whenever the 68000 runs,
 * and delivering at the moment of the request is what the 68000 observes anyway
 * — it raises the request and then spins until the handler acknowledges it. */
void mars_deliver_int(int slave, unsigned level);
#define MARS_INT_CMD 8           /* the 32X command interrupt, on both SH-2s */
#define MARS_INT_PWM 6           /* the PWM timer, which only the slave takes */

/* How many PWM interrupts fall in one 60 Hz frame, from the registers the
 * slave itself programmed. Zero when the timer is off. */
unsigned mars_pwm_per_frame(void);

/* Draw the Mega Drive picture: planes, sprites and palette. */
void genvdp_render(uint32_t *px, unsigned w, unsigned h);

/* The Mega Drive checksum the adapter's boot ROM computes over the cartridge:
 * 16-bit words from 0x200 to the end. It reproduces the header word of both the
 * JU and E images exactly. */
uint16_t mars_rom_checksum(void);

/* Seed the 256 bytes the adapter supplies at 0x000000 — see src/gen68k.c. Must
 * run before the 68000's reset, which fetches its vectors from there. */
void gen68k_init_vectors(void);

/* Let the controller ports forget their TH pulse count, which the real pads do
 * after an idle gap shorter than a frame. */
void gen68k_frame_start(void);

/* One line per 68000 instruction, in the reference tracer's format, for
 * `tools/diff68k.py` to compare against a known-good emulator's log. */
int trace68k_open(const char *path, unsigned long max_lines);
void trace68k_close(void);

/* The same for the SH-2s, one line per basic block entered rather than per
 * instruction — see sh2.h. Both CPUs write to the one file, tagged SHM/SHS as
 * the reference tags its own. */
int sh2_trace_open(const char *path, unsigned long max_lines);
void sh2_trace_close(void);

#endif
