/* Run the machine: Musashi drives the 68000, the recompiled code drives the
 * master SH-2, and the picture — the Mega Drive planes and sprites with the 32X
 * bitmap composited over them — goes to an SDL window.
 *
 * The two CPUs are cooperatively scheduled rather than interleaved: the 68000
 * runs a frame's worth of cycles, and any command it posted to comm register 0
 * is then serviced by calling the SH-2's dispatch loop. That is enough while
 * the SH-2 only ever acts on request; it will need revisiting once timing
 * between the two matters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "mars.h"
#include "m68k.h"

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

/* Entered through sh2_call so tail transfers trampoline rather than nest. */
#define MASTER_RESET 0x060001A0u
#define MASTER_DISPATCH 0x060008F2u
#define SLAVE_RESET 0x060001A4u

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* The sum the adapter's boot ROM computes over the cartridge: 16-bit words from
 * 0x200 to the end, kept to 16 bits. That is the ordinary Mega Drive checksum,
 * and it reproduces the header word of both the JU and E images exactly. */
static uint16_t rom_checksum(void) {
    uint32_t sum = 0;
    for (uint32_t o = 0x200; o + 1 < mars.rom_size; o += 2)
        sum += be16(&mars.rom[o]);
    return (uint16_t)sum;
}

/* What the 32X BIOS posts into the comm registers during boot. We have no BIOS
 * image and start the SH-2s at the cartridge entry points, so these have to be
 * supplied; the reference trace shows all of them written by SH-2 code running
 * below 0x00000300, which is the adapter's boot ROM rather than the cartridge.
 *
 *   comm 4    the cartridge checksum, summed by the master at 0x00000278 and
 *             posted at 0x00000284. The 68000 waits for it at 0x8807C2 and then
 *             compares it against the header's own word at 0x88018E.
 *   comm 0-3  "M_OK" and "S_OK", written by the slave around 0x000001C0. The
 *             68000 waits for those at 0x8809A6 and 0x8809B2.
 *
 * None of it can be posted before the 68000 starts, because the 68000 zeroes
 * these registers itself as part of its own boot — comm 4 at 0x0003F6, comm 0-3
 * at 0x8806F0 — and only then waits for the BIOS to fill them. On hardware the
 * two CPUs are doing this concurrently. Here each value is delivered once, the
 * first time the 68000 is found to have cleared it, which from the 68000's side
 * is indistinguishable. Delivering once and not again matters: the 68000 clears
 * comm 4 again at 0x8807D2 and all of them at 0x880A00 once it is satisfied,
 * and from there on the registers carry real traffic.
 */
static void bios_comm_post(void) {
    static int sum_done, ok_done;
    if (!sum_done && mars.comm[4] == 0) {
        mars.comm[4] = rom_checksum();
        sum_done = 1;
    }
    if (!ok_done && !mars.comm[0] && !mars.comm[1]
                 && !mars.comm[2] && !mars.comm[3]) {
        mars.comm[0] = 0x4D5F; mars.comm[1] = 0x4F4B;   /* M_OK */
        mars.comm[2] = 0x535F; mars.comm[3] = 0x4F4B;   /* S_OK */
        ok_done = 1;
    }
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

int main(int argc, char **argv) {
    const char *rompath = argc > 1 && argv[1][0] != '-' ? argv[1]
        : "roms/Knuckles' Chaotix (JU) (32X) [!].32x";
    int headless_frames = 0;
    const char *trace68k = NULL, *dump_vdp = NULL;
    unsigned long trace68k_lines = 400000;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            headless_frames = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--trace"))
            mars.trace = 1;
        else if (!strcmp(argv[i], "--trace68k") && i + 1 < argc)
            trace68k = argv[++i];
        else if (!strcmp(argv[i], "--trace68k-lines") && i + 1 < argc)
            trace68k_lines = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--dump-vdp") && i + 1 < argc)
            dump_vdp = argv[++i];
        else if (!strcmp(argv[i], "--layers") && i + 1 < argc)
            gen.layers = (unsigned)strtoul(argv[++i], NULL, 0);

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

    SH2 sh2, slave;
    memset(&sh2, 0, sizeof sh2);
    sh2.r[15] = 0x06040000u;
    sh2.vbr = 0x06000000u;
    sh2.pc = start;

    /* The slave has its own vector table at 0x06000080; entry 1 is its stack
     * pointer, exactly as entry 1 of the master's table is the master's. */
    memset(&slave, 0, sizeof slave);
    slave.slave = 1;
    slave.vbr = 0x06000080u;
    slave.r[15] = sh2_r32(&slave, 0x06000084u);

    /* Master init. It ends by waiting for a command, which the watchdog turns
     * into an unwind rather than a hang. */
    printf("SH-2 slave init  (sp=0x%08X) ...\n", slave.r[15]);
    mars_reset_budget(); mars_trace_reset();
    if (setjmp(mars_bail) == 0) sh2_call(&slave, SLAVE_RESET);
    if (mars.trace) mars_trace_dump("after slave init");
    printf("SH-2 master init ...\n");
    mars_reset_budget(); mars_trace_reset();
    if (setjmp(mars_bail) == 0) sh2_call(&sh2, MASTER_RESET);
    if (mars.trace) mars_trace_dump("after master init");
    printf("  bitmap mode 0x%04X, fb control 0x%04X\n",
           mars.bitmap_mode, mars.fbctl);
    printf("  comm: %04X %04X %04X %04X  (want M_OK / S_OK)\n",
           mars.comm[0], mars.comm[1], mars.comm[2], mars.comm[3]);

    printf("  cartridge checksum 0x%04X, header says 0x%04X%s\n",
           rom_checksum(), be16(&mars.rom[H_CHECKSUM]),
           rom_checksum() == be16(&mars.rom[H_CHECKSUM]) ? "" : "  MISMATCH");

    gen68k_init_vectors();
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    /* The 68000 manual leaves the condition codes undefined after reset, and
     * Musashi happens to come up with Z set. Zero them so a trace comparison
     * starts from the same defined state the reference emulator starts from. */
    m68k_set_reg(M68K_REG_SR, 0x2700);
    printf("68000 reset: PC=0x%06X SP=0x%06X\n",
           m68k_get_reg(NULL, M68K_REG_PC), m68k_get_reg(NULL, M68K_REG_SP));
    if (trace68k && !trace68k_open(trace68k, trace68k_lines)) return 1;

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
    }

    int running = 1;
    unsigned frames = 0, serviced = 0;
    unsigned limit = headless_frames ? (unsigned)headless_frames : 0;
    while (running) {
        m68k_execute(CYCLES_PER_FRAME);
        /* A vertical interrupt is what the engine's main loop waits on. */
        m68k_set_irq(6);
        m68k_execute(2000);
        m68k_set_irq(0);

        bios_comm_post();

        while (mars.cmd_at < mars.ncmd) {
            unsigned before = mars.cmd_at;
            mars_reset_budget();
            int why = setjmp(mars_bail);
            if (why == 0) sh2_call(&sh2, MASTER_DISPATCH);
            else mars.bail[why & 3]++;
            serviced++;
            if (mars.cmd_at == before) break;     /* made no progress */
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

    printf("\nafter %u frames:\n", frames);
    printf("  68000 PC=0x%06X   commands posted %u, serviced %u\n",
           m68k_get_reg(NULL, M68K_REG_PC), gen.cmd_posted, serviced);
    printf("  VDP: dma %u, reg1=%02X addr=%04X\n",
           gen.dma_done, gen.vdpreg[1], gen.vdp_addr);
    printf("  32X: bitmap mode 0x%04X  palette %s\n", mars.bitmap_mode,
           mars.cram[0] || mars.cram[3] ? "written" : "all zero");
    /* Enough of the video state to tell an empty frame from a black one: the
     * line table says where each row lives, and pixels beyond it are the image.
     * A frame needs all three — a table, pixels, and a palette. */
    unsigned nz = 0, table = (unsigned)H * 2, pix = 0;
    for (unsigned i = 0; i < MARS_FB; i++)
        if (mars_fb_shown()[i]) { nz++; if (i >= table) pix++; }
    printf("  framebuffer: %u/%u bytes non-zero, %u of them beyond the "
           "%u-byte line table\n", nz, MARS_FB, pix, table);
    unsigned first0 = 256;
    for (unsigned i = 0; i < 256; i++)
        if (!(((unsigned)mars_fb_shown()[i * 2] << 8) | mars_fb_shown()[i * 2 + 1])) {
            first0 = i; break;
        }
    printf("  line table: first zero entry at line %u;", first0);
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
    printf("  unmapped: 68k r=%u w=%u, sh2=%u; dispatch depth bails %u\n",
           gen.unknown_r, gen.unknown_w, mars.unknown, mars.deep);
    printf("  SH-2 unwound: %u idle, %u out of budget, %u too deep\n",
           mars.bail[MARS_BAIL_IDLE], mars.bail[MARS_BAIL_BUDGET],
           mars.bail[MARS_BAIL_DEPTH]);

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
