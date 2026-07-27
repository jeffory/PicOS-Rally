// tuning — all handling constants, parsed from handling.toml at stage start.
// Parser is fs-free (operates on a buffer) so it runs headless and in CI.
#pragma once

#include "mathx.h"

typedef struct {
    // --- vehicle (§9 starting figures) ---
    float mass;           // kg
    float wheelbase;      // m
    float cg_front;       // fraction of wheelbase from front axle to CG
    float cg_height;      // m
    float iz;             // yaw inertia kg·m²
    float max_speed;      // m/s
    float engine_force;   // N at standstill, linear taper to 0 at max_speed
    float brake_force;    // N
    float drag;           // N·s²/m²  (0.4257 ≈ ½ρCdA)
    float rolling_res;    // N·s/m
    float ca_front;       // cornering stiffness N/rad
    float ca_rear;
    // --- digital input (§9: these three ARE the handling on a keyboard) ---
    float steer_ramp_up_s;    // 0→full lock
    float steer_ramp_down_s;  // full→centre
    float steer_max_low_deg;  // lock at zero speed
    float steer_max_high_deg; // lock at max speed
    float steer_curve_knee;   // fraction of max speed where high lock is reached
    float throttle_ramp_up_s;
    float hb_yaw_kick;        // rad/s yaw bias while handbrake + steering
    float hb_mu_cut;          // rear mu multiplier under handbrake
    float hb_ca_cut;          // rear cornering-stiffness multiplier under handbrake
    // --- surfaces ---
    float mu;             // active friction coefficient (F5 cycles in grey box)
    // --- arcade/sim dial (§9) ---
    float assist;         // 0 raw model .. 1 Sega Rally hook-up
    float assist_slip;    // rear slip angle (rad) where yaw assist engages
    float assist_rate;    // yaw blend rate when engaged
} tuning_t;

void tuning_defaults(tuning_t *t);
// Parse "key = value" lines ('#' comments, [sections] ignored). Returns number
// of keys applied; unknown keys are counted in *unknown (may be NULL).
int  tuning_parse(tuning_t *t, const char *buf, int len, int *unknown);
