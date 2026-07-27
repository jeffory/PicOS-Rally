// render_track — stage ground renderer: surface-grid sampler (M3 stand-in
// until the M4 tile blitter). At 4 px/m and 4 m cells, 16 px per cell —
// the sampler is a couple of shifts and a palette load per pixel.
#pragma once

#include "render.h"
#include "track.h"

// Fills the framebuffer with the stage around the camera.
// palette: 8 big-endian colours, indexed by SURF_* (and 6=offworld, 7=unused).
void render_track_ground(framebuf_t *f, const camera_t *cam,
                         const track_t *t, const uint16_t palette[8]);

// Draw the racing line as a thin ribbon (debug/grey-box readability aid).
void render_track_line(framebuf_t *f, const camera_t *cam,
                       const track_t *t, uint16_t be_color);
