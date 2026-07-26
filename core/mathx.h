// mathx — deterministic float math for the rally sim.
// No libm in the hot path: sin/cos from a LUT built with a Taylor polynomial
// (pure mul/add — IEEE-exact and identical on x86-64 and RP2350), atan2 from a
// rational approximation. Determinism is a hard requirement (ghost replays
// and the M3 cross-platform state-hash test depend on it).
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MX_PI 3.14159265358979f
#define MX_TWO_PI 6.28318530717959f

void  mx_init(void);          // build LUTs (call once at startup; deterministic)
float mx_sin(float rad);      // |err| < 2e-4 vs libm
float mx_cos(float rad);
float mx_tan(float rad);      // sin/cos
float mx_atan2(float y, float x);  // |err| < 5e-3 rad
float mx_clamp(float v, float lo, float hi);
float mx_lerp(float a, float b, float t);
// Move cur toward target by at most max_delta (ramp helper).
float mx_move_toward(float cur, float target, float max_delta);
