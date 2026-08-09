/* Boot the recompiled master SH-2 against the 32X memory map and dump what it
 * draws.
 *
 * The 68000 half of the machine does not exist yet, so this stands in for it at
 * exactly the two points where the SH-2 waits on it: the "REDY" handshake word
 * in SDRAM, and the command word in comm register 0. That is enough to get the
 * master through its init and into its command dispatch loop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mars.h"

/* MARS cartridge header, at offset 0x3C0. */
#define H_SRC   0x3D4
#define H_DST   0x3D8
#define H_SIZE  0x3DC
#define H_MSTART 0x3E0

#define CACHE_ROM_SRC 0x07FC00   /* copied to the cache data array at boot */
#define REDY_ADDR     0x06003610

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}

void f_060001A0(SH2 *c);          /* master reset entry, from the recompiler */

/* --- framebuffer output ------------------------------------------------- */
#define W 320
#define H 224

static void put_ppm(const char *path) {
    /* Packed-pixel mode: the framebuffer opens with a table of 16-bit word
     * offsets, one per line, and each line's pixels are palette indices. */
    static uint8_t img[H][W][3];
    unsigned mode = mars.bitmap_mode & 3;
    memset(img, 0, sizeof img);

    for (int y = 0; y < H; y++) {
        uint32_t e = (uint32_t)y * 2;
        uint32_t line = ((uint32_t)mars.fb[e] << 8) | mars.fb[e + 1];
        uint32_t base = (line * 2u) & 0x1FFFFu;
        for (int x = 0; x < W; x++) {
            uint16_t col = 0;
            if (mode == 1) {                       /* packed pixel */
                uint8_t idx = mars.fb[(base + x) & 0x1FFFFu];
                col = ((uint16_t)mars.cram[idx * 2] << 8) | mars.cram[idx * 2 + 1];
            } else if (mode == 2) {                /* direct colour */
                uint32_t o = (base + (uint32_t)x * 2) & 0x1FFFFu;
                col = ((uint16_t)mars.fb[o] << 8) | mars.fb[o + 1];
            }
            /* BGR555, with bit 15 used as a priority flag rather than colour. */
            unsigned r = (col >> 0) & 0x1F, g = (col >> 5) & 0x1F, b = (col >> 10) & 0x1F;
            img[y][x][0] = (uint8_t)(r * 255 / 31);
            img[y][x][1] = (uint8_t)(g * 255 / 31);
            img[y][x][2] = (uint8_t)(b * 255 / 31);
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(img, 1, sizeof img, f);
    fclose(f);
    printf("wrote %s (mode %u)\n", path, mode);
}

static int nonzero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i]) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *rompath = argc > 1 ? argv[1]
        : "roms/Knuckles' Chaotix (JU) (32X) [!].32x";
    const char *out = argc > 2 ? argv[2] : "build/frame.ppm";

    FILE *f = fopen(rompath, "rb");
    if (!f) { perror(rompath); return 1; }
    mars.rom_size = (uint32_t)fread(mars.rom, 1, MARS_ROM_MAX, f);
    fclose(f);
    printf("rom: %s (%u bytes)\n", rompath, mars.rom_size);

    /* What the 32X boot ROM does before the master SH-2 starts. */
    uint32_t src = be32(&mars.rom[H_SRC]);
    uint32_t dst = be32(&mars.rom[H_DST]);
    uint32_t size = be32(&mars.rom[H_SIZE]);
    uint32_t start = be32(&mars.rom[H_MSTART]);
    memcpy(&mars.sdram[dst & 0x3FFFFu], &mars.rom[src], size);
    memcpy(mars.cache, &mars.rom[CACHE_ROM_SRC], sizeof mars.cache);
    printf("sdram image: rom 0x%06X -> 0x%08X (%u bytes)\n",
           src, 0x06000000u + dst, size);

    /* The 68000 signals readiness with this word; the master spins on it. */
    memcpy(&mars.sdram[REDY_ADDR - 0x06000000u], "REDY", 4);

    /* Commands the 68000 would post. 0 is the idle/ack handler, so start with
     * the ones that set the video mode and draw. */
    static const uint16_t script[] = { 1, 2, 3, 4, 5, 7, 9 };
    mars_set_commands(script, sizeof script / sizeof *script);

    SH2 c;
    memset(&c, 0, sizeof c);
    c.r[15] = 0x06040000u;
    c.vbr = 0x06000000u;
    c.pc = start;

    printf("running master SH-2 from 0x%08X ...\n", start);
    int why = setjmp(mars_bail);
    if (why == 0) {
        f_060001A0(&c);
        printf("  master returned\n");
    } else {
        printf("  stopped: %s\n", why == MARS_BAIL_IDLE
               ? "waiting on a command the 68000 would send"
               : "instruction budget exhausted");
    }

    printf("\nafter run:\n");
    printf("  bitmap mode : 0x%04X   fb control : 0x%04X\n",
           mars.bitmap_mode, mars.fbctl);
    printf("  palette     : %s\n", nonzero(mars.cram, sizeof mars.cram)
           ? "written" : "all zero");
    printf("  framebuffer : %s\n", nonzero(mars.fb, MARS_FB)
           ? "written" : "all zero");
    printf("  comm regs   : %04X %04X %04X %04X\n",
           mars.comm[0], mars.comm[1], mars.comm[2], mars.comm[3]);
    printf("  unmapped accesses: %u   missing call targets: %u\n",
           mars.unknown, mars.missing);
    /* Raw framebuffer, so the drawing can be inspected without depending on
     * a palette the 68000 would normally have uploaded. */
    unsigned nz = 0, hist[256] = {0};
    for (unsigned i = 0; i < MARS_FB; i++) { if (mars.fb[i]) nz++; hist[mars.fb[i]]++; }
    unsigned distinct = 0;
    for (int i = 0; i < 256; i++) if (hist[i]) distinct++;
    printf("  fb bytes written: %u/%u, %u distinct values\n", nz, MARS_FB, distinct);
    FILE *rf = fopen("build/fb.bin", "wb");
    if (rf) { fwrite(mars.fb, 1, MARS_FB, rf); fclose(rf); }
    put_ppm(out);
    return 0;
}
