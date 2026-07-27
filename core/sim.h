// sim — fixed-step vehicle simulation. SI units (m, kg, N, rad); render layer
// converts m→px at 4 px/m. Fixed 60 Hz step, float32, LUT trig only.
//
// Model (§9): bicycle with per-axle slip angles, Fy = -Ca·slip clamped to
// mu·load, longitudinal load transfer, linear-taper engine, surface mu.
// Below ~3 m/s the dynamic model is singular → blend to kinematic bicycle.
// The assist dial blends yaw toward velocity heading past a slip threshold.
#pragma once

#include "tuning.h"
#include <stdbool.h>
#include <stdint.h>

#define SIM_DT (1.0f / 60.0f)

typedef struct {
    float throttle;   // 0..1
    float brake;      // 0..1
    float steer;      // -1..1 (left positive)
    bool  handbrake;
} sim_input_t;

typedef struct {
    // pose (world)
    float x, y;       // m
    float heading;    // rad, 0 = +Y (north), clockwise
    // car-frame velocity
    float vx;         // forward m/s
    float vy;         // lateral m/s (+ = left of heading)
    float yaw_rate;   // rad/s
    // ramped inputs
    float steer;      // -1..1
    float throttle;   // 0..1
    float brake;      // 0..1
    // diagnostics (per-step, for HUD/debug)
    float slip_front, slip_rear;  // rad
    float fz_front, fz_rear;      // N
    float accel_lon;              // m/s²
    int   surface;                // last surface index
} car_t;

typedef enum {
    SURF_BITUMEN = 0, SURF_GRAVEL, SURF_SAND, SURF_GRASS, SURF_MUD, SURF_WATER,
    SURF_COUNT
} surface_t;

float sim_surface_mu(int surface);

void sim_init(car_t *car, float x, float y, float heading);
// One 60 Hz step. src: surface provider (per-axle lookup at wheel positions).
// May be NULL → uniform SURF_GRAVEL.
typedef struct surface_src surface_src_t;
void sim_step(car_t *car, const sim_input_t *in, const tuning_t *tun,
              const surface_src_t *src);

// Determinism hash for the M3 replay test (FNV-1a over the POD state).
uint32_t sim_state_hash(const car_t *car);
