/* Run the machine: Musashi drives the 68000, the recompiled code drives the
 * master SH-2, and the picture — the Mega Drive planes and sprites with the 32X
 * bitmap composited over them — goes to an SDL window.
 *
 * The two CPUs are cooperatively scheduled rather than interleaved: the 68000
 * runs a frame's worth of cycles, and the SH-2s run inside its register writes —
 * a command posted to comm register 0, or an interrupt raised at 0xA15102, is
 * handled there and then. That is enough while the SH-2s only ever act on
 * request, and it is what the handshakes need, since the 68000 goes straight on
 * to wait for an answer. It will need revisiting once timing matters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "mars.h"
#include "m68k.h"
#include "m68000.h"

#define H_SRC 0x3D4
#define H_DST 0x3D8
#define H_SIZE 0x3DC
#define H_MSTART 0x3E0
#define H_CHECKSUM 0x18E
#define CACHE_ROM_SRC 0x07FC00
#define REDY_ADDR 0x06003610

#define W 320
#define H 224
#define SCALE 2
#define CYCLES_PER_FRAME 127840      /* 7.67 MHz / 60 */
#define LINES_PER_FRAME 262          /* NTSC */
#define CYCLES_PER_LINE (CYCLES_PER_FRAME / LINES_PER_FRAME)
#define VBLANK_LINE 224              /* where the reference's markers put it */

/* Tried and removed: a VDP bus-contention model, on the theory that the VDP
 * stealing cycles during active display was why our 68000 ran 12,230
 * instructions between vertical interrupts against the reference's 11,232. It
 * changed the count by nothing at all, which is what said the diagnosis was
 * wrong — the budget was not what governed. See cpu_credit below. */

/* The 32X's SH-2s are clocked at 23.01 MHz against the 68000's 7.67 MHz —
 * exactly three times, which is where the ratio comes from rather than a number
 * chosen to look right.
 *
 * The fuel is spent in instructions, so the clock ratio has to be divided by
 * the cycles an instruction actually costs — and that is a measurement, not a
 * constant of the architecture. `tools/refrate.py` counts the reference's own
 * lines between two of the 68000's vertical interrupt markers, which is a frame
 * boundary all three CPUs share, collapsed runs included. Over eighteen steady
 * frames: 11,598 instructions on the 68000, **235,076 on the master and 380,695
 * on the slave**, against 127,840 68000 cycles to the frame and three times
 * that on the SH-2 side.
 *
 * So 1.631 cycles an instruction on the master and 1.007 on the slave. The two
 * differ because their work does: the slave sits in a delay loop in the cache
 * array with nothing to stall on, where the master is decompressing cartridge
 * art. One flat 2 was half the slave's real rate — its whole clock is the PWM
 * interrupt every 1,048 SH-2 cycles, and the reference runs 1,040 instructions
 * between two of them where we ran 524.
 *
 * Per thousand cycles, so the division carries its remainder rather than
 * truncating 4,192 times a frame.
 */
static const unsigned sh2_cpi1000[2] = { 1631, 1007 };

/* How often the CPUs change hands inside a scanline.
 *
 * A whole scanline is far too coarse to be the unit, and the boot says why: the
 * 68000 zeroes comm 0 at 0x880A00, waits for the master's 0xFFFF, and posts its
 * first command at 0x88189C — about sixteen instructions, all inside one
 * scanline's 487 cycles. The master polls that register every six instructions
 * and on hardware samples it some fifty times in that window; run a scanline at
 * a time and it never sees the window at all, so it waits for a zero that has
 * already been and gone.
 *
 * 16 puts a hand-over every ~30 cycles, which is a few 68000 instructions —
 * fine enough for every rendezvous this game makes, and the run is still
 * dominated by the work rather than the switching.
 */
#define SUBSLICES_PER_LINE 16

/* --- which 68000 runs ------------------------------------------------------
 *
 * The recompiled C by default, Musashi under `--interp`. Both drive the same
 * address space — src/gen68k.c — so the only thing that changes is who executes
 * the instructions, and every 32X rendezvous, DMA and interrupt the 68000
 * triggers through a register write happens identically either way.
 *
 * The default flipped once the translated build had a gate of its own rather
 * than "same picture, same command count": `diff68k.py --blocks` holds it to
 * the reference at block granularity and it walks the whole extract, 9,025 of
 * 9,848 block entries agreeing. This is what says the recompiler is the product
 * and the interpreter is the oracle.
 *
 * The interpreter does not go away, and the honest description of what is left
 * for it is not "a fallback" but *the code that did not exist at build time*:
 * the adapter's stubs below 0x100, which src/gen68k.c assembles, and the
 * routines the engine builds in work RAM at 0xFF0000 and jumps to. No static
 * front end can find either. 291 hand-overs in 300 frames, all of them those.
 *
 * The interpreter is paid in cycles and the recompiled code in instructions, so
 * a slice is converted by what an instruction costs. `--recomp-cpi` sets it,
 * and the reference measures it rather than the manual: the 68000 runs 11,236
 * instructions between two vertical interrupts, and a frame is 127,840 cycles,
 * which is 11.4 apiece for this engine's actual mix. It was 8, guessed from the
 * cheapest register-to-register forms being 4 cycles and a memory operand 8 to
 * 12 — and at 8 the recompiled 68000 ran 42% more instructions a frame than the
 * interpreted one did, which is the sort of thing only a trace gate finds.
 *
 * It used to be a count of control *transfers*, which could not bound anything:
 * a poll loop inside one recompiled function is a `goto` and never returns to
 * the trampoline. That was invisible while the SH-2 answered inside the 68000's
 * own register write, and became a hang the moment the two CPUs interleaved.
 */
static int use_recomp = 1;
static unsigned recomp_cpi = 11;
static M68K rcpu;
static uint32_t rc_pc;
static uint32_t rc_missing;      /* transfers to an address with no block */
static uint32_t rc_missing_at;

/* Where the hand-overs go, not just the first one. A million of them a run is
 * either a million real gaps or a handful of addresses hit over and over, and
 * only a count per address tells those apart. A small open-addressed table:
 * this is diagnostics, so an address that does not fit is simply not counted
 * rather than being allowed to evict a hot one. */
#define RC_SITES 512
static struct { uint32_t addr; uint32_t n; } rc_site[RC_SITES];

static void rc_count(uint32_t a) {
    unsigned h = (a * 2654435761u) % RC_SITES;
    for (unsigned k = 0; k < RC_SITES; k++) {
        unsigned i = (h + k) % RC_SITES;
        if (rc_site[i].n == 0) { rc_site[i].addr = a; rc_site[i].n = 1; return; }
        if (rc_site[i].addr == a) { rc_site[i].n++; return; }
    }
}

static void rc_report(void) {
    unsigned shown = 0;
    for (;;) {
        int best = -1;
        for (unsigned i = 0; i < RC_SITES; i++)
            if (rc_site[i].n && (best < 0 || rc_site[i].n > rc_site[best].n))
                best = (int)i;
        if (best < 0 || shown++ >= 8) break;
        printf("      0x%06X  x%u\n", rc_site[best].addr, rc_site[best].n);
        rc_site[best].n = 0;
    }
}

static void cpu_reset(void) {
    /* Musashi is initialised either way: it is what runs the addresses that
     * have no recompiled block, which is code that did not exist at build
     * time. */
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    if (!use_recomp) {
        /* The 68000 manual leaves the condition codes undefined after reset,
         * and Musashi happens to come up with Z set. Zero them so a trace
         * comparison starts from the same defined state the reference emulator
         * starts from. */
        m68k_set_reg(M68K_REG_SR, 0x2700);
        return;
    }
    m68k_reset_recomp(&rcpu, &rc_pc);
}

/* Hand the register file between the two cores.
 *
 * SR goes first because it decides which stack pointer A7 is: Musashi's
 * M68K_REG_SP is whichever one the S bit selects, so setting it before SR would
 * land the value in the wrong register. */
static void to_musashi(void) {
    for (unsigned i = 0; i < 8; i++) {
        m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), rcpu.d[i]);
        m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), rcpu.a[i]);
    }
    m68k_set_reg(M68K_REG_SR, m68k_get_sr(&rcpu));
    m68k_set_reg(M68K_REG_SP, rcpu.a[7]);
    m68k_set_reg(M68K_REG_USP, rcpu.usp);
    m68k_set_reg(M68K_REG_PC, rc_pc);
}

static void from_musashi(void) {
    for (unsigned i = 0; i < 8; i++) {
        rcpu.d[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
        rcpu.a[i] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
    }
    m68k_set_sr(&rcpu, (uint16_t)m68k_get_reg(NULL, M68K_REG_SR));
    rcpu.a[7] = m68k_get_reg(NULL, M68K_REG_SP);
    rcpu.usp = m68k_get_reg(NULL, M68K_REG_USP);
    rc_pc = m68k_get_reg(NULL, M68K_REG_PC);
}

/* Recompiled where there is a block, interpreted where there is not.
 *
 * That is not a stopgap for the front end being incomplete — `coverage` says it
 * has every instruction either trace executes. It is that some of what the
 * 68000 runs is not in the cartridge at all: the engine assembles a handful of
 * routines into work RAM at 0xFF0000 and jumps to them, and no amount of static
 * discovery can find code that does not exist until run time. A recompiler for
 * a machine like this needs an interpreter next to it for exactly those, and
 * the two share an address space and a register file so the handover costs
 * nothing but the copy.
 */
/* Cycles banked from one hand-over to the next.
 *
 * `m68k_execute` runs whole instructions, so it overshoots whatever it was
 * asked for and returns what it actually spent. Discarding that difference is
 * harmless once a scanline and ruinous sixteen times a line: the overshoot, not
 * the request, ends up setting the clock. It cost 8% — the reference runs 11,232
 * instructions between two vertical interrupts and we ran 12,230, stable to the
 * instruction and completely unmoved by changing the cycle budget, which is
 * what says the budget was not what was governing.
 *
 * Carrying the debt makes a slice's request a *rate* again: overspend now and
 * the next slices are shorter until it is paid back.
 */
static int cpu_credit;

/* One hand-over's worth of 68000 cycles, with the remainder distributed rather
 * than truncated. `CYCLES_PER_LINE` is already 487 where the true figure is
 * 487.9, and dividing that by sixteen truncates again to 30 from 30.4 — 2,080
 * cycles a frame between them, which is the 1.8% left after the debt carry
 * above. Handing out CYCLES_PER_FRAME across exactly the frame's hand-overs
 * loses nothing. */
#define STEPS_PER_FRAME (LINES_PER_FRAME * SUBSLICES_PER_LINE)
static unsigned cyc_acc;
static unsigned step_cycles(void) {
    cyc_acc += CYCLES_PER_FRAME;
    unsigned n = cyc_acc / STEPS_PER_FRAME;
    cyc_acc %= STEPS_PER_FRAME;
    return n;
}

/* The same for the SH-2 side, which runs at exactly three times the 68000. It
 * is in cycles rather than instructions because the PWM timer's period is in
 * cycles, and that timer is the whole of the slave's clock. */
static unsigned sh2_cyc_acc;
static unsigned pwm_acc;         /* SH-2 cycles since the last PWM interrupt */
static unsigned sh2_step_cycles(void) {
    sh2_cyc_acc += CYCLES_PER_FRAME * 3;
    unsigned n = sh2_cyc_acc / STEPS_PER_FRAME;
    sh2_cyc_acc %= STEPS_PER_FRAME;
    return n;
}

/* One SH-2's share of a hand-over, in instructions. The two are converted at
 * their own measured rates — see sh2_cpi1000 — and each carries its own
 * remainder, because a sub-slice is some ninety cycles and truncating that
 * division 4,192 times a frame is a percent of the clock. */
static unsigned sh2_insn_acc[2];
static int sh2_step_fuel(int i, unsigned cycles) {
    sh2_insn_acc[i] += cycles * 1000;
    unsigned n = sh2_insn_acc[i] / sh2_cpi1000[i];
    sh2_insn_acc[i] -= n * sh2_cpi1000[i];
    return (int)n;
}

static void cpu_run(unsigned cycles) {
    cpu_credit += (int)cycles;
    if (cpu_credit <= 0) return;             /* still paying off the last one */
    if (!use_recomp) {
        cpu_credit -= m68k_execute(cpu_credit);
        return;
    }
    unsigned slice = (unsigned)cpu_credit / recomp_cpi;
    int known = 1;
    rc_pc = m68k_run(&rcpu, rc_pc, slice ? slice : 1, &known);
    /* What the fuel did not come back with is what the blocks ran. */
    cpu_credit -= (int)(((int)(slice ? slice : 1) - m68k_fuel) * (int)recomp_cpi);
    if (known) return;
    if (!rc_missing++) rc_missing_at = rc_pc;
    rc_count(rc_pc);
    to_musashi();
    /* Run the interpreter to the end of the *gap*, not to the end of the slice.
     *
     * Handing it a whole slice and taking back wherever it stopped is what made
     * this self-sustaining: an arbitrary stopping PC is almost never the start
     * of a block, so the next slice handed over again whether or not anything
     * was really missing. Stopping as soon as the PC is dispatchable turns a
     * gap into one hand-over however long the gap is — which matters most
     * exactly where it was worst, the vblank wait at 0x8834C0, a spin with no
     * recompiled block that was costing a hand-over every sub-slice. */
    do {
        cpu_credit -= m68k_execute(1);
    } while (cpu_credit > 0
             && !m68k_dispatchable((uint32_t)m68k_get_reg(NULL, M68K_REG_PC)));
    from_musashi();
}

/* Offer the pending vertical interrupt, once per hand-over.
 *
 * The request is *held* until the 68000 takes it, which is what the hardware
 * does — the VDP drops it on the acknowledge cycle, in gen68k_int_ack(). It
 * used to be asserted at line 224, given a fixed 2,000 cycles and then lowered,
 * so a 68000 masked across that window lost the frame's vblank outright, and
 * the 2,000 cycles were 1.5% of a frame the machine never had.
 *
 * Musashi latches a level, so setting it repeatedly costs nothing and the
 * ack callback lowers it. The recompiled 68000 has no latch: m68k_interrupt
 * declines while the mask is up, so it is simply offered again next time.
 */
static void cpu_poll_irq(void) {
    if (!gen.vint_irq) return;
    if (!use_recomp) { m68k_set_irq(6); return; }
    if (m68k_interrupt(&rcpu, &rc_pc, 6)) gen.vint_pending = gen.vint_irq = 0;
}

static uint32_t cpu_pc(void) {
    return use_recomp ? rc_pc : m68k_get_reg(NULL, M68K_REG_PC);
}

/* Entered through sh2_call so tail transfers trampoline rather than nest. */
#define MASTER_RESET 0x060001A0u
#define SLAVE_RESET 0x060001A4u

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* The register state the 32X BIOS leaves behind when it hands control to the
 * cartridge, taken from the reference trace's first line at each entry point.
 * We have no BIOS image and start the SH-2s at the cartridge entries, so — like
 * the comm words and the cartridge checksum — this has to be supplied.
 *
 * Three of these are load-bearing rather than cosmetic:
 *
 *   gbr = 0x20004000   the 32X system register block. The cartridge addresses
 *                      the comm registers GBR-relative from 0x06004770 on, and
 *                      the only thing that sets GBR first is `ldc r14,gbr` at
 *                      0x0600592A — with r14 arriving holding that same base.
 *                      Starting at zero sends all of it to address 0.
 *   sr  = 0x000000F1   interrupts masked, T set.
 *   the slave's stack  the cartridge's own slave vector table says 0xC0000800,
 *                      inside the cache data array, which is what we were
 *                      using. The reference shows the slave entering with
 *                      0x0603F800, in SDRAM: the adapter does not take the
 *                      slave's stack from the cartridge table.
 *
 * The rest is scratch the BIOS happened to leave. It is copied verbatim because
 * all-zero is a state the real machine never presents, and because it is what
 * makes a register comparison in tools/diffsh2.py mean something instead of
 * drowning in differences we already know about.
 */
typedef struct { uint32_t r[16], sr, gbr, vbr, pr; } Handoff;

static const Handoff bios_handoff[2] = {
    {   /* master, at 0x060001A0 */
        { 0x4D5F4F4B, 0x04B00000, 0x00000004, 0x0000FFFF,
          0x0000FFFF, 0x00000000, 0x00000001, 0x00000000,
          0x060001A0, 0x06009000, 0x00000000, 0x0000076C,
          0x0000076C, 0x220003E0, 0x20004000, 0x06040000 },
        0x000000F1, 0x20004000, 0x06000000, 0x00000000 },
    {   /* slave, at 0x060001A4 */
        { 0x535F4F4B, 0x00000001, 0x4D5F4F4B, 0x00000000,
          0x00000000, 0x00000000, 0x00000000, 0x00000000,
          0x060001A4, 0xFFFFFE92, 0x00000000, 0x00000000,
          0x00000000, 0x220003E4, 0x20004000, 0x0603F800 },
        0x000000F1, 0x20004000, 0x06000080, 0x000001AE },
};

static void handoff(SH2 *c, int slave) {
    const Handoff *h = &bios_handoff[slave];
    memset(c, 0, sizeof *c);
    memcpy(c->r, h->r, sizeof c->r);
    sh2_set_sr(c, h->sr);
    c->gbr = h->gbr;
    c->vbr = h->vbr;
    c->pr = h->pr;
    c->slave = slave;
}

/* The picture is the Mega Drive's with the 32X bitmap composited over it.
 *
 * Packed pixel and direct colour both start with a per-line table of 16-bit
 * word offsets. Index 0 in packed pixel, and a zero word in direct colour, are
 * transparent and leave the Mega Drive pixel showing — which is currently the
 * whole screen, since the 32X frame buffer holds no pixels yet. The bitmap mode
 * register's priority bit, which can put the 32X behind the Mega Drive instead,
 * is not modelled.
 */
static void render(uint32_t *px) {
    genvdp_render(px, W, H);

    unsigned mode = mars.bitmap_mode & 3;
    if (!(gen.layers & 8)) return;              /* 32X layer switched off */
    if (mode != 1 && mode != 2) return;         /* blank: Mega Drive only */
    const uint8_t *fb = mars_fb_shown();
    for (int y = 0; y < H; y++) {
        uint32_t e = (uint32_t)y * 2;
        uint32_t entry = (((uint32_t)fb[e] << 8) | fb[e + 1]);
        /* A zero entry means the SH-2 never set this line up. Hardware would
         * take it at face value and scan out the line table's own bytes as
         * pixel indices, which is where the rainbow stripes came from; the
         * table is only 128 entries long so far. Showing nothing is the more
         * truthful rendering of a line the game has not drawn, and it keeps the
         * Mega Drive picture — which the game *has* drawn — visible. */
        if (!entry) continue;
        uint32_t base = (entry * 2u) & 0x1FFFFu;
        for (int x = 0; x < W; x++) {
            uint16_t col;
            if (mode == 1) {
                uint8_t idx = fb[(base + (uint32_t)x) & 0x1FFFFu];
                if (!idx) continue;
                col = (uint16_t)((mars.cram[idx * 2] << 8) | mars.cram[idx * 2 + 1]);
            } else {
                uint32_t o = (base + (uint32_t)x * 2) & 0x1FFFFu;
                col = (uint16_t)((fb[o] << 8) | fb[o + 1]);
                if (!col) continue;
            }
            unsigned r = col & 0x1F, g = (col >> 5) & 0x1F, b = (col >> 10) & 0x1F;
            px[y * W + x] = 0xFF000000u | ((r * 255 / 31) << 16)
                          | ((g * 255 / 31) << 8) | (b * 255 / 31);
        }
    }
}

/* The keyboard, as a pad. Held once a frame into gen.pad_buttons and read out
 * of it by src/gen68k.c whenever the game strobes TH.
 *
 * The layout is the one every Mega Drive emulator uses: the three face buttons
 * on the row under the fingers, the three extras above them, and Enter for
 * Start. Mode is on Tab because nothing in this game is known to use it.
 */
static const struct { int key; unsigned bit; } pad_keys[] = {
    { SDL_SCANCODE_UP,     PAD_U }, { SDL_SCANCODE_DOWN,  PAD_D },
    { SDL_SCANCODE_LEFT,   PAD_L }, { SDL_SCANCODE_RIGHT, PAD_R },
    { SDL_SCANCODE_Z,      PAD_A }, { SDL_SCANCODE_X,     PAD_B },
    { SDL_SCANCODE_C,      PAD_C },
    { SDL_SCANCODE_A,      PAD_X }, { SDL_SCANCODE_S,     PAD_Y },
    { SDL_SCANCODE_D,      PAD_Z },
    { SDL_SCANCODE_RETURN, PAD_S }, { SDL_SCANCODE_TAB,   PAD_M },
};

/* The same twelve by name, for `--hold up,start` — which is how a headless run
 * gets to press anything, and the only way the input path can be tested without
 * a person at the keyboard. */
static const struct { const char *name; unsigned bit; } pad_names[] = {
    { "up", PAD_U }, { "down", PAD_D }, { "left", PAD_L }, { "right", PAD_R },
    { "a", PAD_A }, { "b", PAD_B }, { "c", PAD_C }, { "start", PAD_S },
    { "x", PAD_X }, { "y", PAD_Y }, { "z", PAD_Z }, { "mode", PAD_M },
};

static unsigned parse_hold(const char *s) {
    unsigned held = 0;
    while (*s) {
        size_t n = strcspn(s, ",");
        for (unsigned i = 0; i < sizeof pad_names / sizeof *pad_names; i++)
            if (n == strlen(pad_names[i].name) && !strncmp(s, pad_names[i].name, n))
                held |= pad_names[i].bit;
        s += n + (s[n] == ',');
    }
    return held;
}

static unsigned read_pad(void) {
    SDL_PumpEvents();
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    unsigned held = 0;
    for (unsigned i = 0; i < sizeof pad_keys / sizeof *pad_keys; i++)
        if (k[pad_keys[i].key]) held |= pad_keys[i].bit;
    /* Opposite directions at once is something a real d-pad cannot do and some
     * engines handle badly; drop both rather than pick one. */
    if ((held & (PAD_L | PAD_R)) == (PAD_L | PAD_R)) held &= ~(PAD_L | PAD_R);
    if ((held & (PAD_U | PAD_D)) == (PAD_U | PAD_D)) held &= ~(PAD_U | PAD_D);
    return held;
}

int main(int argc, char **argv) {
    const char *rompath = argc > 1 && argv[1][0] != '-' ? argv[1]
        : "roms/Knuckles' Chaotix (JU) (32X) [!].32x";
    int headless_frames = 0;
    const char *trace68k = NULL, *dump_vdp = NULL, *tracesh2 = NULL;
    const char *dump_32x = NULL;
    unsigned long trace68k_lines = 400000, tracesh2_lines = 400000;
    unsigned hold = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            headless_frames = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--trace"))
            mars.trace = 1;
        else if (!strcmp(argv[i], "--trace68k") && i + 1 < argc)
            trace68k = argv[++i];
        else if (!strcmp(argv[i], "--trace68k-lines") && i + 1 < argc)
            trace68k_lines = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--trace-sh2") && i + 1 < argc)
            tracesh2 = argv[++i];
        else if (!strcmp(argv[i], "--trace-sh2-lines") && i + 1 < argc)
            tracesh2_lines = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--dump-vdp") && i + 1 < argc)
            dump_vdp = argv[++i];
        else if (!strcmp(argv[i], "--dump-32x") && i + 1 < argc)
            dump_32x = argv[++i];
        else if (!strcmp(argv[i], "--layers") && i + 1 < argc)
            gen.layers = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc)
            hold = parse_hold(argv[++i]);
        else if (!strcmp(argv[i], "--recomp"))
            use_recomp = 1;                 /* the default; kept so it still runs */
        else if (!strcmp(argv[i], "--interp"))
            use_recomp = 0;
        else if (!strcmp(argv[i], "--recomp-cpi") && i + 1 < argc)
            recomp_cpi = (unsigned)strtoul(argv[++i], NULL, 0);

    if (!gen.layers) gen.layers = 15;   /* planes, sprites, 32X bitmap */

    FILE *f = fopen(rompath, "rb");
    if (!f) { perror(rompath); return 1; }
    mars.rom_size = (uint32_t)fread(mars.rom, 1, MARS_ROM_MAX, f);
    fclose(f);

    /* What the 32X boot ROM stages before either CPU starts. */
    uint32_t src = be32(&mars.rom[H_SRC]), dst = be32(&mars.rom[H_DST]);
    uint32_t size = be32(&mars.rom[H_SIZE]), start = be32(&mars.rom[H_MSTART]);
    memcpy(&mars.sdram[dst & 0x3FFFFu], &mars.rom[src], size);
    memcpy(mars.cache, &mars.rom[CACHE_ROM_SRC], sizeof mars.cache);
    memcpy(&mars.sdram[REDY_ADDR - 0x06000000u], "REDY", 4);
    printf("rom %u bytes; sdram image 0x%06X -> 0x%08X (%u)\n",
           mars.rom_size, src, 0x06000000u + dst, size);

    SH2 *sh2 = &mars_cpu[0], *slave = &mars_cpu[1];
    handoff(sh2, 0);
    handoff(slave, 1);
    if (start != MASTER_RESET)
        printf("  ! header master start 0x%08X, entering at 0x%08X\n",
               start, MASTER_RESET);

    /* Opened before either SH-2 runs: the boot is the part with an oracle. */
    if (tracesh2 && !sh2_trace_open(tracesh2, tracesh2_lines)) return 1;

    /* Neither SH-2 starts here. They are held in the adapter's boot ROM — which
     * the runtime stands in for — and handed to the cartridge when that work is
     * done, then run from the frame loop like the 68000. They used to be run to
     * completion before the 68000 existed at all, and the reference shows what
     * that cost: the engine's own 32X init toggles the frame-select bit while
     * the master is still working, where ours had already finished. */
    mars_trace_reset();
    printf("SH-2s held in BIOS (slave sp=0x%08X)\n", slave->r[15]);

    printf("  cartridge checksum 0x%04X, header says 0x%04X%s\n",
           mars_rom_checksum(), be16(&mars.rom[H_CHECKSUM]),
           mars_rom_checksum() == be16(&mars.rom[H_CHECKSUM]) ? "" : "  MISMATCH");

    gen68k_init_vectors();
    cpu_reset();
    /* And the VDP's pending vertical interrupt is already up before the
     * cartridge runs an instruction: the adapter's boot ROM took 379,000 of
     * them to get here, masked at level 7 the whole way, so every vblank in
     * there set a flag nothing acknowledged. The reference's first status read,
     * at 0x0005B0, has it set. */
    gen.vint_pending = 1;
    printf("68000 reset: PC=0x%06X  (%s)\n", cpu_pc(),
           use_recomp ? "recompiled" : "Musashi");
    if (trace68k && !trace68k_open(trace68k, trace68k_lines, use_recomp))
        return 1;

    SDL_Window *win = NULL; SDL_Renderer *ren = NULL; SDL_Texture *tex = NULL;
    static uint32_t px[W * H];
    if (!headless_frames) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
        win = SDL_CreateWindow("Knuckles' Chaotix (32X, recompiled)",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               W * SCALE, H * SCALE, 0);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, W, H);
        printf("pad: arrows, Z/X/C = A/B/C, A/S/D = X/Y/Z, Enter = Start, "
               "Tab = Mode, Esc quits\n");
    }

    int running = 1;
    unsigned frames = 0;
    unsigned limit = headless_frames ? (unsigned)headless_frames : 0;
    while (running) {
        /* The frame is run in slices with a PWM interrupt between each, rather
         * than as one block with the interrupts bunched at the end. The slave's
         * sound driver is the only thing that takes them and it does a fixed
         * amount of work per interrupt, so what the spacing buys is not audio
         * timing — it is that the two CPUs interleave the way the reference
         * shows them interleaving, which is what the trace comparison sees. */
        /* The frame is run a scanline at a time, and everything timed hangs off
         * that one clock: the vertical interrupt when the line reaches 224, the
         * VDP's own VBLANK bit so a wait loop sees the same thing, and the
         * slave's PWM interrupts spread across the lines they fall in.
         *
         * The reference's own interrupt markers are what fixes the line: after
         * the display is up they all read "Vblank SR=3 @ 224,n". Taking it at
         * the end of the frame instead — which is what this did — put every
         * post-boot comparison out by however far into its wait loop the engine
         * had got. */
        gen68k_frame_start();
        gen.pad_buttons = headless_frames ? hold : (read_pad() | hold);
        for (unsigned line = 0; line < LINES_PER_FRAME; line++) {
            gen.line = line;
            /* Raised as the beam reaches the line, before the 68000 runs any of
             * it, which is where the reference's own markers put it —
             * "Vblank SR=3 @ 224,n". It stays raised until acknowledged. */
            if (line == VBLANK_LINE) gen.vint_pending = gen.vint_irq = 1;

            /* The three CPUs take turns through the line. A rendezvous costs
             * the waiting one a hand-over or two rather than being answered
             * inside its own register write, which is the whole point: the
             * reference's master is busy through 22 of its 102 vblanks, and the
             * engine skips work in those. */
            for (unsigned s = 0; s < SUBSLICES_PER_LINE; s++) {
                cpu_poll_irq();
                cpu_run(step_cycles());

                unsigned sh2c = sh2_step_cycles();
                sh2_run(&mars_cpu[0], sh2_step_fuel(0, sh2c));

                /* The slave's whole clock. Its interrupts used to be counted
                 * per frame and spread over the scanlines, which quantised each
                 * one to a line boundary and dropped the fractional interrupt —
                 * 365 of the 365.96 the registers ask for. Counting SH-2 cycles
                 * against the period leaves neither error, and no extra slice
                 * is handed out for taking one: the interrupt only redirects
                 * the CPU, and the fuel below is what runs the handler. */
                unsigned period = mars_pwm_period();
                if (period) {
                    pwm_acc += sh2c;
                    while (pwm_acc >= period) {
                        pwm_acc -= period;
                        mars_deliver_int(1, MARS_INT_PWM);
                        mars.pwm_ints++;
                    }
                }
                sh2_run(&mars_cpu[1], sh2_step_fuel(1, sh2c));
            }
        }

        frames++;
        if (!headless_frames) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
                if (ev.type == SDL_QUIT ||
                    (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
                    running = 0;
            render(px);
            SDL_UpdateTexture(tex, NULL, px, W * 4);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }
        if (limit && frames >= limit) running = 0;
    }

    trace68k_close();
    sh2_trace_close();
    if (mars.trace) mars_trace_dump("after the frame loop");

    /* Everything the Mega Drive picture is made of, so a renderer bug can be
     * worked on against a fixed snapshot instead of a moving target. */
    if (dump_vdp) {
        FILE *d = fopen(dump_vdp, "wb");
        if (d) {
            /* CRAM and VSRAM are 16-bit arrays; write them big-endian so the
             * file reads the same way the VDP's own words do, rather than
             * however this host happens to order bytes. */
            fwrite(gen.vram, 1, sizeof gen.vram, d);
            for (unsigned i = 0; i < 64; i++)
                fputc(gen.cram[i] >> 8, d), fputc(gen.cram[i] & 0xFF, d);
            for (unsigned i = 0; i < 40; i++)
                fputc(gen.vsram[i] >> 8, d), fputc(gen.vsram[i] & 0xFF, d);
            fwrite(gen.vdpreg, 1, sizeof gen.vdpreg, d);
            fclose(d);
            printf("  wrote %s (vram, cram, vsram, regs)\n", dump_vdp);
        }
    }

    /* The 32X half of the picture, in the layout tools/refframe.py rebuilds
     * from the reference's own stores: both frame buffers, then the palette,
     * then the two registers that say how to read them. Comparing the two is
     * the only check the rendering path has ever had against ground truth. */
    if (dump_32x) {
        FILE *d = fopen(dump_32x, "wb");
        if (d) {
            fwrite(mars.fb[0], 1, MARS_FB, d);
            fwrite(mars.fb[1], 1, MARS_FB, d);
            fwrite(mars.cram, 1, sizeof mars.cram, d);
            fputc(mars.fbctl >> 8, d);       fputc(mars.fbctl & 0xFF, d);
            fputc(mars.bitmap_mode >> 8, d); fputc(mars.bitmap_mode & 0xFF, d);
            fclose(d);
            printf("  wrote %s (fb0, fb1, cram, fbctl, bitmap mode)\n", dump_32x);
        }
    }

    printf("\nafter %u frames:\n", frames);
    printf("  68000 PC=0x%06X   commands posted %u, serviced %u  (",
           cpu_pc(), gen.cmd_posted, mars.serviced);
    for (unsigned i = 0; i < 16; i++)
        if (gen.cmd_hist[i]) printf("%u:%u ", i, gen.cmd_hist[i]);
    printf(")\n");
    printf("  comm: %04X %04X %04X %04X   DMA words to SH-2: %u\n",
           mars.comm[0], mars.comm[1], mars.comm[2], mars.comm[3],
           mars.dma_words);
    printf("  PWM: ctl %04X cycle %04X -> %u int/frame, %u delivered\n",
           mars.pwm_ctl, mars.pwm_cycle, mars_pwm_per_frame(), mars.pwm_ints);
    printf("  VDP: dma %u, reg1=%02X addr=%04X\n",
           gen.dma_done, gen.vdpreg[1], gen.vdp_addr);
    /* Whether the palette has been uploaded at all, asked of the whole of it:
     * sampling two bytes said "all zero" the moment entry 0 became a legitimate
     * transparent black, which is what the SEGA logo's own palette starts with. */
    unsigned cused = 0;
    for (unsigned i = 0; i < sizeof mars.cram; i++) if (mars.cram[i]) cused++;
    printf("  32X: bitmap mode 0x%04X  palette %s\n", mars.bitmap_mode,
           cused ? "written" : "all zero");
    /* Enough of the video state to tell an empty frame from a black one: the
     * line table says where each row lives, and pixels beyond it are the image.
     * A frame needs all three — a table, pixels, and a palette. */
    unsigned nz = 0, table = (unsigned)H * 2, pix = 0;
    for (unsigned i = 0; i < MARS_FB; i++)
        if (mars_fb_shown()[i]) { nz++; if (i >= table) pix++; }
    printf("  framebuffer: %u/%u bytes non-zero, %u of them beyond the "
           "%u-byte line table\n", nz, MARS_FB, pix, table);
    /* A line whose entry is zero is one the SH-2 never set up, and the renderer
     * leaves it showing the Mega Drive. "First zero" is not the measure — the
     * table can have a hole in it and be fine either side — so count how many of
     * the 224 lines on screen have somewhere to scan out from, and say whether
     * they all point at the same place, which is what a collapsed table looks
     * like. */
    unsigned set = 0, distinct = 0, seen[H];
    for (unsigned i = 0; i < H; i++) {
        unsigned e = ((unsigned)mars_fb_shown()[i * 2] << 8)
                   | mars_fb_shown()[i * 2 + 1];
        if (!e) continue;
        set++;
        unsigned j = 0;
        while (j < distinct && seen[j] != e) j++;
        if (j == distinct) seen[distinct++] = e;
    }
    printf("  line table: %u/%u lines set, %u distinct offset(s);", set, H, distinct);
    for (unsigned i = 0; i < 6; i++)
        printf(" %04X", ((unsigned)mars_fb_shown()[i * 2] << 8)
                       | mars_fb_shown()[i * 2 + 1]);
    printf("\n  palette:  ");
    for (unsigned i = 0; i < 6; i++)
        printf(" %04X", ((unsigned)mars.cram[i * 2] << 8) | mars.cram[i * 2 + 1]);
    unsigned cnz = 0;
    for (unsigned i = 0; i < sizeof mars.cram; i++) if (mars.cram[i]) cnz++;
    printf("   (%u/%u bytes non-zero)\n", cnz, (unsigned)sizeof mars.cram);

    /* The Mega Drive half has its own picture, and this game may well put the
     * first one there rather than in the 32X framebuffer. */
    unsigned vnz = 0, gnz = 0;
    for (unsigned i = 0; i < sizeof gen.vram; i++) if (gen.vram[i]) vnz++;
    for (unsigned i = 0; i < 64; i++) if (gen.cram[i]) gnz++;
    printf("  genesis: vram %u/%u bytes non-zero, cram %u/64 entries set\n",
           vnz, (unsigned)sizeof gen.vram, gnz);
    unsigned pa = (unsigned)(gen.vdpreg[2] & 0x38) << 10;
    unsigned pb = (unsigned)(gen.vdpreg[4] & 0x07) << 13;
    unsigned na = 0, nb = 0;
    for (unsigned i = 0; i < 0x1000; i += 2) {
        if (gen.vram[(pa + i) & 0xFFFF] || gen.vram[(pa + i + 1) & 0xFFFF]) na++;
        if (gen.vram[(pb + i) & 0xFFFF] || gen.vram[(pb + i + 1) & 0xFFFF]) nb++;
    }
    printf("           display %s, planes A=%04X (%u/2048 cells) "
           "B=%04X (%u/2048), sprites=%04X, size reg=%02X\n",
           (gen.vdpreg[1] & 0x40) ? "on" : "off", pa, na, pb, nb,
           (unsigned)(gen.vdpreg[5] & 0x7F) << 9, gen.vdpreg[16]);
    printf("           regs:");
    for (unsigned i = 0; i < 24; i++) printf(" %02X", gen.vdpreg[i]);
    printf("\n");
    printf("  unmapped: 68k r=%u w=%u, sh2=%u\n",
           gen.unknown_r, gen.unknown_w, mars.unknown);
    printf("  SH-2 parked at: master 0x%08X, slave 0x%08X\n",
           mars_cpu[0].pc, mars_cpu[1].pc);
    if (use_recomp) {
        printf("  recompiled 68000: %u handover(s) to the interpreter",
               rc_missing);
        if (rc_missing) printf(", first 0x%06X", rc_missing_at);
        printf("\n");
        if (rc_missing) rc_report();
    }

    if (headless_frames) {
        render(px);
        FILE *o = fopen("build/frame.ppm", "wb");
        if (o) {
            fprintf(o, "P6\n%d %d\n255\n", W, H);
            for (int i = 0; i < W * H; i++) {
                uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8),
                                   (uint8_t)px[i] };
                fwrite(rgb, 1, 3, o);
            }
            fclose(o);
            printf("  wrote build/frame.ppm\n");
        }
    } else {
        SDL_DestroyTexture(tex); SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win); SDL_Quit();
    }
    return 0;
}
