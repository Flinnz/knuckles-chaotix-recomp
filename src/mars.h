/* Machine state for the 32X side: memory, VDP registers, and the comm
 * registers through which the (not yet present) 68000 drives the SH-2s. */
#ifndef MARS_H
#define MARS_H

#include <stdint.h>
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

    /* 32X system registers.
     *
     * `bank` and `hcount` sit at the same offset in the two register blocks and
     * are *different registers*: the 68000's 0xA15104 selects which megabyte of
     * the cartridge shows at 0x900000, and the SH-2's 0x20004004 is the H Count
     * — how many scanlines apart the horizontal interrupt comes. Sharing one
     * field meant the SH-2 setting its H count silently moved the 68000's
     * cartridge window back to bank 0, which is where the engine reads its
     * palettes from. */
    uint16_t adapter, intctl, bank, hcount;
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
     * it, so these two registers are what its clock is made of — and what its
     * sample rate is made of too. The unit itself, its FIFOs and everything
     * downstream of them live in src/sound.c. */
    uint16_t pwm_ctl, pwm_cycle;
    uint32_t pwm_ints;       /* delivered, for reporting */

    /* 32X VDP registers */
    uint16_t bitmap_mode, shift, fill_len, fill_start, fill_data, fbctl;
    /* SH-2 cycles left on an autofill. The VDP is busy with the frame buffer
     * while it runs and says so through FEN, which the master polls after every
     * fill. See vdp_status() in src/mem32x.c for where the rate comes from. */
    uint32_t fen_left;
    /* And what the fills have cost altogether, because the master waits for
     * every one of them and nothing else in the report would show it. A fill
     * length taken as sixteen bits where the register has eight had the master
     * spending 90% of its cycles on FEN after the SEGA logo, and the only
     * visible symptom was a game that stopped moving. */
    uint32_t fills, fill_words;

    uint32_t ticks;          /* stands in for elapsed time when polling */
    uint32_t unknown;        /* accesses outside the modelled map */
    uint32_t missing;        /* indirect transfers with no recompiled target */
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
    uint32_t hpos;           /* how far into it, 0-255, one step a hand-over */
    /* What a DMA owes the 68000, in thousandths of a scanline.
     *
     * A DMA holds the 68000 off the bus for as long as it runs, and the VDP's
     * own table gives that as *transfers per scanline* — so a scanline is the
     * natural unit for src/gen68k.c to hand back and cycles are the frame
     * loop's business, which is where the clock constants live. Charged and
     * cleared by cpu_run(). */
    uint32_t dma_lines1000;
    /* Two different things that used to be one flag. `vint_pending` is VDP
     * status bit 7, which the adapter's boot ROM leaves set before the
     * cartridge runs an instruction — 379,000 instructions of vblanks nothing
     * acknowledged — and which the reference reads back set at its very first
     * status read. `vint_irq` is the request on the 68000's own interrupt
     * lines, and only the frame loop raises that. Conflating them meant the
     * boot's stale status bit fired an interrupt the moment the 68000 dropped
     * its mask, which lands in the window before the engine has installed a
     * handler: our first vblank went to the do-nothing `rte` at 0x880B2A,
     * which the reference never executes. */
    uint8_t  vint_pending;   /* VDP status bit 7, until the 68000 acknowledges */
    uint8_t  vint_irq;       /* the request line, until the acknowledge cycle */
    uint8_t  pad_cycle[3], pad_th[3];   /* six-button pad sequencing */
    unsigned pad_buttons;    /* what player one is holding; see PAD_* below */
    unsigned layers;         /* 1 plane B, 2 plane A, 4 sprites */
} Gen;

extern Mars mars;
extern Gen gen;

/* The twelve lines of a six-button pad, as `gen.pad_buttons` holds them. Set
 * from the keyboard once a frame in src/mars_main.c; nothing sets it in a
 * headless run, so every trace comparison still sees an untouched pad. */
#define PAD_U 0x001u
#define PAD_D 0x002u
#define PAD_L 0x004u
#define PAD_R 0x008u
#define PAD_A 0x010u
#define PAD_B 0x020u
#define PAD_C 0x040u
#define PAD_S 0x080u    /* Start */
#define PAD_X 0x100u
#define PAD_Y 0x200u
#define PAD_Z 0x400u
#define PAD_M 0x800u    /* Mode */

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
void mars_trace_dump(const char *why);
void mars_trace_reset(void);

/* The two SH-2s live here rather than in main() so that the runtime can enter
 * one from anywhere — specifically from a 68000 register write, which is what
 * raising an interrupt is. */
extern SH2 mars_cpu[2];          /* 0 = master, 1 = slave */

/* Where the adapter's boot ROM hands each SH-2 over to the cartridge. */
#define MARS_MASTER_ENTRY 0x060001A0u
#define MARS_SLAVE_ENTRY  0x060001A4u

/* Start them there. Until this is called they are held, which is not a
 * convenience: on hardware both SH-2s are inside the adapter's boot ROM for the
 * machine's first 379,000 instructions, and that ROM is what computes the
 * cartridge checksum and posts M_OK / S_OK. The runtime stands in for all of
 * it, so the hand-over is when that stand-in has finished — see
 * bios_comm_read() in src/gen68k.c. Starting them at reset instead lets the
 * master's dispatch loop read M_OK out of comm 0 and dispatch on it. */
void mars_bios_handover(void);

/* Raise an external interrupt on one SH-2: it stacks SR and PC and comes back
 * with the CPU pointed at its handler, which runs on that CPU's own slices.
 * Declined if the CPU's mask is at or above the level. */
void mars_deliver_int(int slave, unsigned level);
#define MARS_INT_CMD 8           /* the 32X command interrupt, on both SH-2s */
#define MARS_INT_PWM 6           /* the PWM timer, which only the slave takes */

/* The same, for an interrupt one CPU raises inside another's hand-over.
 *
 * `at` is how far into the target's own slice for this hand-over the raise
 * happened, in that CPU's cycles — `mars_slice_pos()` converts the 68000's
 * position into it. The frame loop splits the slice there rather than letting
 * the target act on the interrupt from the start of a window the raise was
 * three quarters of the way through. See the note in src/mem32x.c for the
 * measurement that says it matters.
 */
void mars_raise_int(int slave, unsigned level, unsigned at);
unsigned mars_int_due(int slave, unsigned cycles);   /* cycles before it lands */
void mars_int_fire(int slave);                       /* deliver what is due */
void mars_int_rebase(unsigned elapsed);              /* a hand-over has passed */

/* Where the 68000 stands inside the current hand-over, in the SH-2s' cycles.
 * Lives in src/mars_main.c, which is the only thing that knows the window. */
unsigned mars_slice_pos(void);

/* One PWM cycle in SH-2 cycles, and the timer interrupt's period, which is TM
 * of those — both from the registers the slave itself programmed. src/sound.c
 * counts SH-2 cycles against the first; the second is the only clock the slave
 * has. Zero when the unit, or the interrupt, is off. */
unsigned mars_pwm_sample_period(void);
unsigned mars_pwm_period(void);

/* The sample rate the cycle register works out to, in Hz. */
unsigned mars_pwm_rate(void);

/* The same thing as a count per frame, for the end-of-run report only: it is
 * 365.96 here, so it is not a quantity to schedule from. */
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

/* What the 68000 got through, which is the question `sh2_insns` already asks of
 * the SH-2s and for the same reason: the reference retires 11,611 instructions
 * a frame in the steady state and about 9,830 through the boot, where it sits
 * in a 13-cycle poll of a 32X register, and a build whose clock is wrong shows
 * up here as the wrong rate long before a trace diff could say so.
 *
 * Bumped from two places because there are two 68000s. The recompiled build
 * knows what it ran from the fuel it did not spend; Musashi is counted through
 * its instruction hook, which the tracer also installs — so the increment lives
 * in that hook too rather than being lost whenever `--trace68k` is on. */
extern unsigned long cpu_insns;

/* Whether the tracers are recording.
 *
 * Every gate in this project reads a trace that starts at the machine's reset,
 * because that is where the reference logs start. Nothing could look at a *late*
 * window — and the game stops working well past the end of the reference, so
 * "what is the engine doing at frame 1,200" had no instrument at all. The frame
 * loop arms this at `--trace-from`, and all four tracers honour it.
 */
extern int trace_armed;

/* One line per 68000 instruction, in the reference tracer's format, for
 * `tools/diff68k.py` to compare against a known-good emulator's log. */
/* `blocks` records one line per recompiled basic block entered instead of one
 * per interpreted instruction — the only granularity the translated build can
 * be hooked at, and what `tools/diff68k.py --blocks` compares. */
int trace68k_open(const char *path, unsigned long max_lines, int blocks);
void trace68k_close(void);

/* The same for the SH-2s, one line per basic block entered rather than per
 * instruction — see sh2.h. Both CPUs write to the one file, tagged SHM/SHS as
 * the reference tags its own. */
int sh2_trace_open(const char *path, unsigned long max_lines);
void sh2_trace_close(void);

#endif
