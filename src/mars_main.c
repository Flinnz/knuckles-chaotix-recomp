/* Run the machine: Musashi drives the 68000, the recompiled code drives the
 * master SH-2, and the 32X framebuffer goes to an SDL window.
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

/* Convert the 32X framebuffer to RGB. Packed pixel and direct colour both
 * start with a per-line table of 16-bit word offsets. */
static void render(uint32_t *px) {
    unsigned mode = mars.bitmap_mode & 3;
    for (int y = 0; y < H; y++) {
        uint32_t e = (uint32_t)y * 2;
        uint32_t base = ((((uint32_t)mars.fb[e] << 8) | mars.fb[e + 1]) * 2u) & 0x1FFFFu;
        for (int x = 0; x < W; x++) {
            uint16_t col = 0;
            if (mode == 1) {
                uint8_t idx = mars.fb[(base + (uint32_t)x) & 0x1FFFFu];
                col = (uint16_t)((mars.cram[idx * 2] << 8) | mars.cram[idx * 2 + 1]);
            } else if (mode == 2) {
                uint32_t o = (base + (uint32_t)x * 2) & 0x1FFFFu;
                col = (uint16_t)((mars.fb[o] << 8) | mars.fb[o + 1]);
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
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            headless_frames = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--trace"))
            mars.trace = 1;

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

    /* The 32X BIOS, which runs on both SH-2s out of the adapter's own boot ROM
     * at 0x00000000, performs a ready handshake with the 68000 before cartridge
     * code takes over: it posts "M_OK" and "S_OK" into the comm registers, and
     * the 68000 spins at 0x8809A6 until it sees them. We have no BIOS image and
     * start the SH-2s at the cartridge entry points instead, so the handshake
     * is supplied here. Confirmed against a reference trace, where those words
     * are written by the slave executing at 0x000001C0 - BIOS space, not SDRAM.
     */
    mars.comm[0] = 0x4D5F; mars.comm[1] = 0x4F4B;   /* M_OK */
    mars.comm[2] = 0x535F; mars.comm[3] = 0x4F4B;   /* S_OK */
    printf("posted BIOS handshake: M_OK / S_OK\n");

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    printf("68000 reset: PC=0x%06X SP=0x%06X\n",
           m68k_get_reg(NULL, M68K_REG_PC), m68k_get_reg(NULL, M68K_REG_SP));

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

        while (mars.cmd_at < mars.ncmd) {
            unsigned before = mars.cmd_at;
            mars_reset_budget();
            if (setjmp(mars_bail) == 0) sh2_call(&sh2, MASTER_DISPATCH);
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

    printf("\nafter %u frames:\n", frames);
    printf("  68000 PC=0x%06X   commands posted %u, serviced %u\n",
           m68k_get_reg(NULL, M68K_REG_PC), gen.cmd_posted, serviced);
    printf("  VDP: dma %u, reg1=%02X addr=%04X\n",
           gen.dma_done, gen.vdpreg[1], gen.vdp_addr);
    printf("  32X: bitmap mode 0x%04X  palette %s\n", mars.bitmap_mode,
           mars.cram[0] || mars.cram[3] ? "written" : "all zero");
    unsigned nz = 0;
    for (unsigned i = 0; i < MARS_FB; i++) if (mars.fb[i]) nz++;
    printf("  framebuffer: %u/%u bytes non-zero\n", nz, MARS_FB);
    printf("  unmapped: 68k r=%u w=%u, sh2=%u; dispatch depth bails %u\n",
           gen.unknown_r, gen.unknown_w, mars.unknown, mars.deep);

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
