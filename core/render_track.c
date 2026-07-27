#include "render_track.h"
#include "tiles_sections.h"

// M4 tile blitter. The world is axis-aligned: one screen offset derived from
// the camera, then uniform 16 px steps — no per-pixel transform.
void render_track_ground_gfx(gfx_t *g, uint16_t *fb, const camera_t *cam,
                             const track_t *t, int vp_h) {
    if (!t->tilemap) {
        framebuf_t f = { fb, GFX_FB_W, vp_h };
        static const uint16_t fallback_pal[8] = {
            0x5146, 0x6104, 0xCE59, 0x3B48, 0x5965, 0x2C54, 0x18C3, 0x18C3 };
        render_track_ground(&f, cam, t, fallback_pal);
        return;
    }
    const int ppm = RENDER_PX_PER_M;             // 4 px per m
    const int cell_px = (int)(t->cell * ppm);    // 16
    // screen px of the grid origin (rounded once — tiles never jitter)
    const int ox_sx = (int)((t->ox - cam->x) * ppm + (GFX_FB_W / 2) + 0.5f);
    const int oy_sy = (int)((t->oy - cam->y) * ppm + (vp_h / 2) + 0.5f);
    // visible cell window (1 cell slack for partial tiles at edges); cells
    // outside the grid render as grass fill so the viewport is always full
    int cx0 = (-ox_sx) / cell_px - 1;
    int cy0 = (-oy_sy) / cell_px - 1;
    int cx1 = (GFX_FB_W - ox_sx) / cell_px + 1;
    int cy1 = (vp_h - oy_sy) / cell_px + 1;

    for (int cy = cy0; cy <= cy1; cy++) {
        int sy = oy_sy + cy * cell_px;
        const uint16_t *row = (cy >= 0 && cy < t->gh)
            ? t->tilemap + cy * t->gw * TRACK_TILEMAP_SLOTS : 0;
        for (int cx = cx0; cx <= cx1; cx++) {
            int sx = ox_sx + cx * cell_px;
            if (!row || cx < 0 || cx >= t->gw) {
                gfx_blit_tile(g, fb, vp_h, TILE_GRASS_FILL, sx, sy);
                continue;
            }
            const uint16_t *slots = row + cx * TRACK_TILEMAP_SLOTS;
            for (int s = 0; s < TRACK_TILEMAP_SLOTS - 1; s++) {
                if (slots[s] != TRACK_NO_TILE)
                    gfx_blit_tile(g, fb, vp_h, slots[s], sx, sy);
            }
            if (slots[TRACK_TILEMAP_SLOTS - 1] != TRACK_NO_TILE)
                gfx_blit_tile_masked(g, fb, vp_h,
                                     slots[TRACK_TILEMAP_SLOTS - 1], sx, sy);
        }
    }
}

void render_track_props_gfx(gfx_t *g, uint16_t *fb, const camera_t *cam,
                            const track_t *t, int vp_h,
                            const uint8_t *props_bin, int prop_size) {
    if (!props_bin || !t->props) return;
    const int ppm = RENDER_PX_PER_M;
    const int half = prop_size / 2;
    for (int i = 0; i < t->num_props; i++) {
        const track_prop_t *p = &t->props[i];
        int sx = (int)((p->x_dm * 0.1f - cam->x) * ppm + (GFX_FB_W / 2)) - half;
        int sy = (int)((p->y_dm * 0.1f - cam->y) * ppm + (vp_h / 2)) - half;
        if (sx < -prop_size || sy < -prop_size ||
            sx >= GFX_FB_W || sy >= vp_h) continue;
        gfx_blit_sprite(g, fb, vp_h,
                        props_bin + p->type * prop_size * prop_size,
                        prop_size, prop_size, sx, sy);
    }
}

void render_track_ground(framebuf_t *f, const camera_t *cam,
                         const track_t *t, const uint16_t palette[8]) {
    // world px (4 per m) of the screen's top-left corner
    const int base_wx = (int)(cam->x * RENDER_PX_PER_M) - f->w / 2;
    const int base_wy = (int)(cam->y * RENDER_PX_PER_M) - f->h / 2;
    // grid cell (4 m = 16 px) of the grid origin, in world px
    const int cell_px = (int)(t->cell * RENDER_PX_PER_M);
    const int ox_px = (int)(t->ox * RENDER_PX_PER_M);
    const int oy_px = (int)(t->oy * RENDER_PX_PER_M);

    for (int sy = 0; sy < f->h; sy++) {
        int wy = base_wy + sy;
        int cy = (wy - oy_px) / cell_px;
        const uint8_t *grow = 0;
        if (cy >= 0 && cy < t->gh) grow = t->grid + cy * t->gw;
        uint16_t *dst = f->fb + sy * f->w;
        int wx = base_wx;
        for (int sx = 0; sx < f->w; sx++, wx++) {
            if (!grow) { *dst++ = palette[3]; continue; }   // outside grid: grass
            int cx = (wx - ox_px) / cell_px;
            int s = (cx >= 0 && cx < t->gw) ? grow[cx] : 3;
            *dst++ = palette[s & 7];
        }
    }
}

void render_track_line(framebuf_t *f, const camera_t *cam,
                       const track_t *t, uint16_t be_color) {
    const int cx0 = f->w / 2, cy0 = f->h / 2;
    for (int i = 0; i < t->num_points; i++) {
        int sx = (int)((t->points[i].x - cam->x) * RENDER_PX_PER_M) + cx0;
        int sy = (int)((t->points[i].y - cam->y) * RENDER_PX_PER_M) + cy0;
        if ((unsigned)sx < (unsigned)f->w && (unsigned)sy < (unsigned)f->h)
            f->fb[sy * f->w + sx] = be_color;
    }
}
