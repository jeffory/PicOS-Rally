// gfx — see gfx.h. Hot loops: per-pixel work is fetch-idx + CLUT + store.
#include "gfx.h"
#include "font6x8.h"

void gfx_init(gfx_t *g, const uint16_t *clut,
              const uint8_t **sections, const int *bases, const int *counts,
              int n_sections) {
    g->clut = clut;
    for (int i = 0; i < 256; i++) g->gtile[i] = 0;
    for (int s = 0; s < n_sections; s++) {
        const uint8_t *sec = sections[s];
        for (int t = 0; t < counts[s]; t++) {
            int gi = bases[s] + t;
            if (gi >= 0 && gi < 256) g->gtile[gi] = sec + t * (GFX_TILE * GFX_TILE);
        }
    }
}

void gfx_blit_tile(gfx_t *g, uint16_t *fb, int fb_h,
                   int gidx, int sx, int sy) {
    const uint8_t *tile = g->gtile[gidx & 0xFF];
    if (!tile) return;
    const uint16_t *clut = g->clut;
    for (int ty = 0; ty < GFX_TILE; ty++) {
        int y = sy + ty;
        if (y < 0 || y >= fb_h) continue;
        uint16_t *row = fb + y * GFX_FB_W;
        const uint8_t *trow = tile + ty * GFX_TILE;
        for (int tx = 0; tx < GFX_TILE; tx++) {
            int x = sx + tx;
            if (x < 0 || x >= GFX_FB_W) continue;
            row[x] = clut[trow[tx]];
        }
    }
}

void gfx_blit_tile_masked(gfx_t *g, uint16_t *fb, int fb_h,
                          int gidx, int sx, int sy) {
    const uint8_t *tile = g->gtile[gidx & 0xFF];
    if (!tile) return;
    const uint16_t *clut = g->clut;
    for (int ty = 0; ty < GFX_TILE; ty++) {
        int y = sy + ty;
        if (y < 0 || y >= fb_h) continue;
        uint16_t *row = fb + y * GFX_FB_W;
        const uint8_t *trow = tile + ty * GFX_TILE;
        for (int tx = 0; tx < GFX_TILE; tx++) {
            int x = sx + tx;
            if (x < 0 || x >= GFX_FB_W) continue;
            uint8_t v = trow[tx];
            if (v != GFX_TRANSPARENT) row[x] = clut[v];
        }
    }
}

void gfx_blit_sprite(gfx_t *g, uint16_t *fb, int fb_h,
                     const uint8_t *spr, int w, int h,
                     int sx, int sy) {
    const uint16_t *clut = g->clut;
    for (int ty = 0; ty < h; ty++) {
        int y = sy + ty;
        if (y < 0 || y >= fb_h) continue;
        uint16_t *row = fb + y * GFX_FB_W;
        const uint8_t *srow = spr + ty * w;
        for (int tx = 0; tx < w; tx++) {
            int x = sx + tx;
            if (x < 0 || x >= GFX_FB_W) continue;
            uint8_t v = srow[tx];
            if (v != GFX_TRANSPARENT) row[x] = clut[v];
        }
    }
}

void gfx_fill(gfx_t *g, uint16_t *fb, int fb_h,
              int x, int y, int w, int h, uint8_t pal) {
    uint16_t c = g->clut[pal];
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= fb_h) continue;
        uint16_t *row = fb + yy * GFX_FB_W;
        for (int xx = x; xx < x + w; xx++)
            if (xx >= 0 && xx < GFX_FB_W) row[xx] = c;
    }
}

int gfx_text_width(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n * 6;
}

void gfx_text(gfx_t *g, uint16_t *fb, int fb_h,
              int x, int y, const char *s, uint8_t pal) {
    gfx_text_scale(g, fb, fb_h, x, y, s, pal, 1);
}

void gfx_text_scale(gfx_t *g, uint16_t *fb, int fb_h,
                    int x, int y, const char *s, uint8_t pal, int scale) {
    uint16_t c = g->clut[pal];
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    for (; *s; s++, x += 6 * scale) {
        if (*s < 0x20 || *s > 0x7E) continue;
        const uint8_t *glyph = s_font6x8[*s - 0x20];  // 6 column bytes, LSB=top
        for (int gx = 0; gx < 6; gx++) {
            uint8_t bits = glyph[gx];
            for (int gy = 0; gy < 8; gy++) {
                if (!(bits & (1 << gy))) continue;
                // draw the scale×scale block, clipped
                for (int dy = 0; dy < scale; dy++) {
                    int yy = y + gy * scale + dy;
                    if (yy < 0 || yy >= fb_h) continue;
                    uint16_t *row = fb + yy * GFX_FB_W;
                    for (int dx = 0; dx < scale; dx++) {
                        int xx = x + gx * scale + dx;
                        if (xx >= 0 && xx < GFX_FB_W) row[xx] = c;
                    }
                }
            }
        }
    }
}
