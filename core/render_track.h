// render_track — stage ground renderer. M4: baked-tilemap blitter through
// the gfx 8bpp pipeline (replaces the M3 surface-grid sampler, kept below
// as a fallback for v1 blobs / headless).
#pragma once

#include "render.h"
#include "track.h"
#include "gfx.h"

// M4: blit the baked tilemap around the camera into rows [0, vp_h).
// Cells are 4 m = 16 px at 4 px/m, exactly one tile — the whole ground is
// ~21x16 tile blits per frame. Falls back to the surface sampler on v1 blobs.
void render_track_ground_gfx(gfx_t *g, uint16_t *fb, const camera_t *cam,
                             const track_t *t, int vp_h);

// M4: blit scattered props (masked sprites, world-anchored).
void render_track_props_gfx(gfx_t *g, uint16_t *fb, const camera_t *cam,
                            const track_t *t, int vp_h,
                            const uint8_t *props_bin, int prop_size);

// Fills the framebuffer with the stage around the camera (M3 stand-in).
// palette: 8 big-endian colours, indexed by SURF_* (and 6=offworld, 7=unused).
void render_track_ground(framebuf_t *f, const camera_t *cam,
                         const track_t *t, const uint16_t palette[8]);

// Draw the racing line as a thin ribbon (debug/grey-box readability aid).
void render_track_line(framebuf_t *f, const camera_t *cam,
                       const track_t *t, uint16_t be_color);