#include "render.h"
#include "font6x8.h"

// 6×8 text into the framebuffer. Bit order matches the firmware font:
// glyph[col], bit N = row N (top = bit0). bg < 0 = transparent.
void render_text(framebuf_t *f, int x, int y, const char *text,
                 uint16_t be_fg, int32_t be_bg) {
    for (; *text; text++) {
        char c = *text;
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *glyph = s_font6x8[c - 0x20];
        for (int col = 0; col < FONT6_W; col++) {
            uint8_t coldata = glyph[col];
            int px = x + col;
            if ((unsigned)px >= (unsigned)f->w) continue;
            for (int row = 0; row < FONT6_H; row++) {
                int py = y + row;
                if ((unsigned)py >= (unsigned)f->h) continue;
                if (coldata & (1 << row))
                    f->fb[py * f->w + px] = be_fg;
                else if (be_bg >= 0)
                    f->fb[py * f->w + px] = (uint16_t)be_bg;
            }
        }
        x += FONT6_W;
    }
}

// Solid rect helper (grey-box overlays).
void render_fill_rect(framebuf_t *f, int x, int y, int w, int h, uint16_t be) {
    for (int yy = y; yy < y + h; yy++) {
        if ((unsigned)yy >= (unsigned)f->h) continue;
        for (int xx = x; xx < x + w; xx++)
            if ((unsigned)xx < (unsigned)f->w)
                f->fb[yy * f->w + xx] = be;
    }
}

void render_clear(framebuf_t *f, uint16_t be_color) {
    uint32_t v = ((uint32_t)be_color << 16) | be_color;
    uint32_t *p = (uint32_t *)f->fb;
    int n = (f->w * f->h) / 2;
    for (int i = 0; i < n; i++) p[i] = v;
}

void render_line(framebuf_t *f, int x0, int y0, int x1, int y1, uint16_t be) {
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        if ((unsigned)x0 < (unsigned)f->w && (unsigned)y0 < (unsigned)f->h)
            f->fb[y0 * f->w + x0] = be;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

// Ground blit: world texel (wx, wy) = pixel at (cam*4 + screen - centre).
// The texture is painted with the world origin at its centre (surface_paint_
// texture), so the sampling offset includes the half-texture centre term —
// without it the visual terrain and the physics surface map are 64 m apart.
void render_ortho_ground(framebuf_t *f, const camera_t *cam,
                         const uint16_t *tex, int tw, int th) {
    const int cx = f->w / 2, cy = f->h / 2;
    const int base_wx = (int)(cam->x * RENDER_PX_PER_M) - cx + tw / 2;
    const int base_wy = (int)(cam->y * RENDER_PX_PER_M) - cy + th / 2;
    const uint32_t wmask = (uint32_t)tw - 1;   // caller guarantees pow2
    const uint32_t hmask = (uint32_t)th - 1;

    for (int sy = 0; sy < f->h; sy++) {
        uint32_t wy = (uint32_t)(base_wy + sy) & hmask;
        const uint16_t *src_row = tex + wy * tw;
        uint16_t *dst = f->fb + sy * f->w;
        uint32_t wx = (uint32_t)base_wx & wmask;
        int remaining = f->w;
        while (remaining > 0) {
            int run = tw - (int)wx;
            if (run > remaining) run = remaining;
            const uint16_t *s = src_row + wx;
            for (int i = 0; i < run; i++) {
                uint16_t c = s[i];
                dst[i] = (uint16_t)((c >> 8) | (c << 8));   // host → big-endian
            }
            dst += run;
            remaining -= run;
            wx = 0;
        }
    }
}

void render_car(framebuf_t *f, const camera_t *cam, const car_t *car,
                uint16_t be_body, uint16_t be_outline) {
    const int cx = f->w / 2, cy = f->h / 2;
    int sx = (int)((car->x - cam->x) * RENDER_PX_PER_M) + cx;
    int sy = (int)((car->y - cam->y) * RENDER_PX_PER_M) + cy;

    // Car: 4.2 m long × 1.8 m wide → 17 × 7 px (§6).
    const int hw = 3, hh = 8;   // half width/length in px (length along heading)
    float sh = mx_sin(car->heading), ch = mx_cos(car->heading);

    // corners (fwd = (sh, ch) in screen px: screen y down = +y world)
    int fx = (int)(sh * hh), fy = (int)(ch * hh);
    int rx = (int)(ch * hw), ry = (int)(-sh * hw);

    // filled body: axis-aligned rect at screen pos (grey box — reads fine)
    for (int y = sy - 2; y <= sy + 2; y++)
        for (int x = sx - 5; x <= sx + 5; x++)
            if ((unsigned)x < (unsigned)f->w && (unsigned)y < (unsigned)f->h)
                f->fb[y * f->w + x] = be_body;

    // rotated outline shows heading
    int x0 = sx + fx + rx, y0 = sy + fy + ry;
    int x1 = sx + fx - rx, y1 = sy + fy - ry;
    int x2 = sx - fx - rx, y2 = sy - fy - ry;
    int x3 = sx - fx + rx, y3 = sy - fy + ry;
    render_line(f, x0, y0, x1, y1, be_outline);
    render_line(f, x1, y1, x2, y2, be_outline);
    render_line(f, x2, y2, x3, y3, be_outline);
    render_line(f, x3, y3, x0, y0, be_outline);
}
