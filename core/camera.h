// camera — world-fixed follow cam with velocity look-ahead (§10).
// No zoom (no scaling budget): at speed we shift the centre further ahead
// instead — same readability effect, zero cost.
#pragma once

#include "sim.h"

typedef struct {
    float x, y;         // world position the camera centres on (m)
    float shake_x, shake_y;
} camera_t;

void camera_init(camera_t *cam, const car_t *car);
// Follow with v·0.35 s look-ahead, lead clamped so the car never sits closer
// than ~2.5 s of road to the screen edge (§10), capped to ±14 m (56 px).
void camera_update(camera_t *cam, const car_t *car, float dt);

// Impact shake, decaying, capped at 3 px (0.75 m) — call camera_kick on hits.
void camera_kick(camera_t *cam, float sx, float sy);
