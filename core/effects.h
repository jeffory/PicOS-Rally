// effects — M5 feel: dust particles + skid marks. Visual only (never fed
// back into the sim, so M3 determinism is unaffected). No PicOS headers.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "sim.h"
#include "camera.h"
#include "gfx.h"

#define FX_MAX_PARTICLES 64
#define FX_SKID_SEGS 256

typedef struct {
    float x, y;          // world m
    float vx, vy;        // world velocity m/s
    uint8_t life;        // sim steps remaining
    uint8_t life0;       // initial life (fade reference)
    uint8_t pal;         // CLUT base index (surface-tinted at spawn)
    uint8_t size;        // px (2 or 3)
} fx_particle_t;

typedef struct {
    int16_t x1_dm, y1_dm, x2_dm, y2_dm;   // world decimeters
} fx_skid_seg_t;

typedef struct {
    fx_particle_t p[FX_MAX_PARTICLES];
    uint32_t rng;
    float spawn_acc;
    // skids
    fx_skid_seg_t segs[FX_SKID_SEGS];
    int skid_head;
    float skid_acc;
    float last_rx, last_ry;
    bool has_last;
} fx_t;

void fx_init(fx_t *fx, uint32_t seed);

// Per sim step: spawns dust from the rear axle driven by throttle/slip/
// surface, lays skid segments when sliding, ages existing particles.
void fx_step(fx_t *fx, const car_t *car);

// Render dust (screen space, under the car sprite).
void fx_render_particles(const fx_t *fx, gfx_t *g, uint16_t *fb,
                         const camera_t *cam, int vp_h);

// Render skid marks (world-anchored dark dots, under the car sprite).
void fx_render_skids(const fx_t *fx, gfx_t *g, uint16_t *fb,
                     const camera_t *cam, int vp_h);
