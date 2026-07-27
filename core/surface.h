// surface — world→surface providers. M2's procedural test oval and M3's
// baked track grid both implement one interface; the sim takes it.
#pragma once

#include <stdint.h>

// Generic surface source: fn(ctx, x, y) → SURF_*.
typedef struct surface_src {
    int (*at)(void *ctx, float x, float y);
    void *ctx;
} surface_src_t;

// ── M2 procedural test oval ──────────────────────────────────────────────────
typedef struct surface_map {
    int unused;
} surface_map_t;

void surface_init_test_oval(surface_map_t *m);
int  surface_at(const surface_map_t *m, float x, float y);
// Provider wrapper (ctx = surface_map_t*).
surface_src_t surface_src_oval(surface_map_t *m);

// Rolling-resistance multiplier per surface (bitumen = 1.0).
float surface_rr_scale(int surface);

// Paint the oval into a host-order RGB565 texture (grey box ground).
void surface_paint_texture(const surface_map_t *m, uint16_t *tex,
                           int tex_w, int tex_h, int px_per_m);
