// surface — world→surface lookup. M2: a procedural test oval (so surface
// transitions are feelable before the real stage exists). M3 replaces the
// implementation with the baked stage01 grid; the interface stays.
//
// The test oval, centred on the world origin (metres, heading convention
// 0=+Y north, clockwise):
//   ring track: radius 50–60 m
//     [  0°,  90°): gravel        [ 90°, 180°): bitumen (township)
//     [180°, 360°): gravel
//   creek: water band, angle 225° ± 3°, radius 46–64 m (crosses the track)
//   sand trap: [180°, 270°), radius 60–66 m (outside runoff)
//   everything else: grass
#pragma once

#include <stdint.h>

typedef struct surface_map {
    int unused;   // M3: baked grid pointer/dims go here
} surface_map_t;

void surface_init_test_oval(surface_map_t *m);

// SURF_* (sim.h) at world position (x, y) in metres.
int  surface_at(const surface_map_t *m, float x, float y);

// Rolling-resistance multiplier per surface (bitumen = 1.0).
float surface_rr_scale(int surface);

// Paint the surface map into a host-order RGB565 texture, tex_px per metre,
// map centred at the texture centre. For the grey box ground.
void surface_paint_texture(const surface_map_t *m, uint16_t *tex,
                           int tex_w, int tex_h, int px_per_m);
