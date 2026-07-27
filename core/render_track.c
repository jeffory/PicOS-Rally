#include "render_track.h"

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
