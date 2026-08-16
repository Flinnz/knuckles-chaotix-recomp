/* Rendering the Mega Drive picture.
 *
 * The 32X composites its bitmap with the Mega Drive's, and in this game the
 * first picture is entirely on the Mega Drive side: by the time the boot
 * settles, 45 KB of tiles, a full 64-colour palette and both name tables are
 * populated while the 32X frame buffer still holds nothing but its line table.
 * Without this the window stays black no matter how well the CPUs run.
 *
 * Scanline at a time, and only what this game actually selects is exercised —
 * H40, two 64x32 planes, per-line horizontal scroll, whole-screen vertical
 * scroll, sprites, shadow/highlight — but the register decoding is general so
 * the other modes behave rather than being silently ignored. Not modelled: the
 * window plane, interlace, and the per-line sprite and pixel limits that real
 * hardware runs out of.
 */
#include <string.h>
#include "mars.h"

/* Plane size in cells, indexed by the two-bit field. The 10 encoding is not
 * legal; hardware behaves as 32, so that is what it does here. */
static const unsigned PLANE_CELLS[4] = { 32, 64, 32, 128 };

#define VRAM16(a) (((unsigned)gen.vram[(a) & 0xFFFEu] << 8) \
                 | gen.vram[((a) & 0xFFFEu) + 1])

/* 0000 BBB0 GGG0 RRR0 — three bits a channel. */
static uint32_t colour(unsigned entry) {
    uint16_t c = gen.cram[entry & 0x3F];
    unsigned r = (c >> 1) & 7, g = (c >> 5) & 7, b = (c >> 9) & 7;
    return 0xFF000000u | ((r * 255 / 7) << 16) | ((g * 255 / 7) << 8)
         | (b * 255 / 7);
}

typedef struct { uint8_t idx, pal, pri; } Px;

/* --- shadow and highlight ---------------------------------------------------
 *
 * With bit 3 of register 12 set the picture has three intensities instead of
 * one, and two sprite colours stop being colours: palette 3 index 15 shadows
 * what is under it and index 14 highlights it, drawing nothing themselves. The
 * background is at low intensity wherever neither plane put a priority pixel,
 * and a sprite with priority is always normal.
 *
 * This game turns it on for one room — the Combi Catcher, at 0x89 for some 300
 * frames — and drawing the operators as though they were colours is what put
 * solid black bars where its light beams are and black ellipses under every
 * character and lamp. Palette 3's entries 14 and 15 are not written by a game
 * that uses them as operators, so they were black.
 *
 * Shadow halves the channel and highlight is the top half of the range, which
 * is the hardware's own arithmetic: three bits become four, 0-7 and 8-15.
 */
static uint32_t shade(uint32_t c, int shadow, int hilite) {
    if (shadow == hilite) return c;              /* both, or neither, is normal */
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    if (shadow) { r >>= 1; g >>= 1; b >>= 1; }
    else { r = (r >> 1) + 128; g = (g >> 1) + 128; b = (b >> 1) + 128; }
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* One pixel of a scrolling plane. `hs` scrolls the picture right, `vs` down. */
static Px plane_pixel(unsigned base, unsigned cw, unsigned ch,
                      int hs, int vs, unsigned x, unsigned y) {
    unsigned px = (unsigned)((int)x - hs), py = (unsigned)((int)y + vs);
    unsigned col = (px >> 3) & (cw - 1), row = (py >> 3) & (ch - 1);
    unsigned cell = VRAM16(base + ((row * cw + col) * 2));
    unsigned fx = px & 7, fy = py & 7;
    if (cell & 0x1000) fy = 7 - fy;                 /* vflip */
    if (cell & 0x0800) fx = 7 - fx;                 /* hflip */
    unsigned b = gen.vram[((cell & 0x7FF) * 32 + fy * 4 + (fx >> 1)) & 0xFFFFu];
    Px p;
    p.idx = (uint8_t)((fx & 1) ? (b & 0xF) : (b >> 4));
    p.pal = (uint8_t)((cell >> 13) & 3);
    p.pri = (uint8_t)((cell >> 15) & 1);
    return p;
}

/* Sprites covering one scanline, in link order — the first sprite to claim a
 * pixel keeps it, which is how hardware resolves overlap. */
static void sprite_line(Px *line, unsigned w, unsigned y, int h40) {
    unsigned base = (unsigned)gen.vdpreg[5] << 9;
    unsigned max = h40 ? 80u : 64u;
    unsigned idx = 0;
    for (unsigned n = 0; n < max; n++) {
        unsigned e = (base + idx * 8) & 0xFFFFu;
        int sy = (int)(VRAM16(e) & 0x3FF) - 128;
        unsigned size = gen.vram[(e + 2) & 0xFFFFu];
        unsigned hs = ((size >> 2) & 3) + 1, vs = (size & 3) + 1;
        unsigned link = gen.vram[(e + 3) & 0xFFFFu] & 0x7F;
        unsigned attr = VRAM16(e + 4);
        int sx = (int)(VRAM16(e + 6) & 0x1FF) - 128;

        int dy = (int)y - sy;
        if (dy >= 0 && dy < (int)(vs * 8)) {
            unsigned tile = attr & 0x7FF, pal = (attr >> 13) & 3;
            unsigned pri = (attr >> 15) & 1;
            unsigned fy = (unsigned)dy;
            if (attr & 0x1000) fy = vs * 8 - 1 - fy;          /* vflip */
            for (unsigned cx = 0; cx < hs; cx++) {
                unsigned scx = (attr & 0x0800) ? hs - 1 - cx : cx;  /* hflip */
                /* Cells of a sprite run down each column before moving right. */
                unsigned t = tile + scx * vs + (fy >> 3);
                for (unsigned px = 0; px < 8; px++) {
                    int ox = sx + (int)(cx * 8 + px);
                    if (ox < 0 || ox >= (int)w) continue;
                    unsigned fx = (attr & 0x0800) ? 7 - px : px;
                    unsigned b = gen.vram[((t & 0x7FF) * 32 + (fy & 7) * 4
                                           + (fx >> 1)) & 0xFFFFu];
                    unsigned v = (fx & 1) ? (b & 0xF) : (b >> 4);
                    if (v && !line[ox].idx) {
                        line[ox].idx = (uint8_t)v;
                        line[ox].pal = (uint8_t)pal;
                        line[ox].pri = (uint8_t)pri;
                    }
                }
            }
        }
        if (!link)
            break;
        idx = link;
    }
}

void genvdp_render(uint32_t *px, unsigned w, unsigned h, uint8_t *opaque) {
    uint32_t back = colour(gen.vdpreg[7] & 0x3F);
    if (opaque) memset(opaque, 0, w * h);
    if (!(gen.vdpreg[1] & 0x40)) {              /* display disabled */
        for (unsigned i = 0; i < w * h; i++) px[i] = back;
        return;                                 /* all backdrop, all see-through */
    }

    int h40 = gen.vdpreg[12] & 1;
    unsigned visible = h40 ? 320u : 256u;
    if (visible > w) visible = w;
    unsigned pa = (unsigned)(gen.vdpreg[2] & 0x38) << 10;
    unsigned pb = (unsigned)(gen.vdpreg[4] & 0x07) << 13;
    unsigned hsb = (unsigned)(gen.vdpreg[13] & 0x3F) << 10;
    unsigned cw = PLANE_CELLS[gen.vdpreg[16] & 3];
    unsigned ch = PLANE_CELLS[(gen.vdpreg[16] >> 4) & 3];
    unsigned hmode = gen.vdpreg[11] & 3;        /* 0 whole, 2 per cell, 3 per line */
    int vcol = (gen.vdpreg[11] >> 2) & 1;       /* vertical scroll per 2 cells */
    int sh = (gen.vdpreg[12] >> 3) & 1;         /* shadow/highlight */

    /* One scanline of sprite pixels. 320 is the widest the Mega Drive shows. */
    Px sprites[320];
    if (visible > sizeof sprites / sizeof *sprites)
        visible = sizeof sprites / sizeof *sprites;

    for (unsigned y = 0; y < h; y++) {
        unsigned hse = hsb + (hmode == 3 ? y * 4
                            : hmode == 2 ? (y & ~7u) * 4 : 0);
        int hsa = (int)(VRAM16(hse) & 0x3FF);
        int hsb_ = (int)(VRAM16(hse + 2) & 0x3FF);

        memset(sprites, 0, sizeof sprites);
        if (gen.layers & 4) sprite_line(sprites, visible, y, h40);

        for (unsigned x = 0; x < w; x++) {
            if (x >= visible) { px[y * w + x] = back; continue; }
            /* Vertical scroll comes from VSRAM: one pair of entries for the
             * whole screen, or a pair per two cells across. */
            unsigned vi = vcol ? ((x >> 4) * 2) : 0;
            /* 40 entries, so bound rather than mask: a 320-pixel line asks for
             * index 40 at the right-hand edge, and masking would read past the
             * array instead of wrapping. */
            int vsa = (int)(gen.vsram[vi % 40] & 0x3FF);
            int vsb = (int)(gen.vsram[(vi + 1) % 40] & 0x3FF);

            Px zero = { 0, 0, 0 };
            Px a = (gen.layers & 2) ? plane_pixel(pa, cw, ch, hsa, vsa, x, y)
                                    : zero;
            Px b = (gen.layers & 1) ? plane_pixel(pb, cw, ch, hsb_, vsb, x, y)
                                    : zero;
            Px s = sprites[x];

            int shadow = 0, hilite = 0;
            if (sh) {
                /* Low intensity wherever no plane claimed priority — the
                 * backdrop included. */
                shadow = !((a.idx && a.pri) || (b.idx && b.pri));
                if (s.idx && s.pal == 3 && s.idx >= 14) {
                    if (s.idx == 14) hilite = 1; else shadow = 1;
                    s.idx = 0;                  /* an operator draws nothing */
                } else if (s.idx && s.pri) {
                    shadow = 0;                 /* a sprite with priority is normal */
                }
            }

            /* Back to front: backdrop, then each layer's low-priority pixels,
             * then the same three again for the ones marked high. */
            unsigned e = gen.vdpreg[7] & 0x3F;
            if (b.idx && !b.pri) e = b.pal * 16 + b.idx;
            if (a.idx && !a.pri) e = a.pal * 16 + a.idx;
            if (s.idx && !s.pri) e = s.pal * 16 + s.idx;
            if (b.idx && b.pri)  e = b.pal * 16 + b.idx;
            if (a.idx && a.pri)  e = a.pal * 16 + a.idx;
            if (s.idx && s.pri)  e = s.pal * 16 + s.idx;
            px[y * w + x] = shade(colour(e), shadow, hilite);
            /* Whether anything at all was drawn here, which is a different
             * question from what colour came out: a plane pixel may resolve to
             * the same colour as the backdrop and is still not the backdrop.
             * The 32X needs the distinction — when the Mega Drive has priority
             * it shows only through the pixels the Mega Drive left alone. */
            if (opaque)
                opaque[y * w + x] = (uint8_t)(a.idx || b.idx || s.idx);
        }
    }
}
