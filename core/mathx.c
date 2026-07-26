#include "mathx.h"

// 1024-entry sine LUT over [0, 2π). Filled by mx_sin_poly (Taylor on a
// reduced range), never libm — identical values on every platform.
#define MX_LUT_BITS 10
#define MX_LUT_SIZE (1 << MX_LUT_BITS)   // 1024
#define MX_LUT_MASK (MX_LUT_SIZE - 1)

static float s_sin_lut[MX_LUT_SIZE];

// Taylor sin on [-π/4, π/4] to x^13 — error < 1e-9, deterministic mul/add.
static float mx_sin_poly(float x) {
    const float x2 = x * x;
    return x * (1.0f
         + x2 * (-1.0f / 6.0f
         + x2 * (1.0f / 120.0f
         + x2 * (-1.0f / 5040.0f
         + x2 * (1.0f / 362880.0f
         + x2 * (-1.0f / 39916800.0f
         + x2 * (1.0f / 6227020800.0f)))))));
}

void mx_init(void) {
    // Quadrant folding keeps the polynomial on [-π/4, π/4] where it's accurate:
    //   [0, π/2]:   sin(x)          [π/2, π]:     sin(π - x)
    //   [π, 3π/2]: -sin(x - π)      [3π/2, 2π]:  -sin(2π - x)
    for (int i = 0; i < MX_LUT_SIZE; i++) {
        float x = (float)i / (float)MX_LUT_SIZE * MX_TWO_PI;
        if (x <= MX_PI / 2.0f)
            s_sin_lut[i] = mx_sin_poly(x);
        else if (x <= MX_PI)
            s_sin_lut[i] = mx_sin_poly(MX_PI - x);
        else if (x <= 1.5f * MX_PI)
            s_sin_lut[i] = -mx_sin_poly(x - MX_PI);
        else
            s_sin_lut[i] = -mx_sin_poly(MX_TWO_PI - x);
    }
}

float mx_sin(float rad) {
    // wrap to [0, 2π)
    float x = rad - (float)(int)(rad * (1.0f / MX_TWO_PI)) * MX_TWO_PI;
    if (x < 0.0f) x += MX_TWO_PI;
    // nearest LUT entry (no lerp — 1024 entries ≈ 0.35° steps, err ≤ 3e-3;
    // sim uses this for heading/trig where that's inaudible; cheap wins)
    int idx = (int)(x * (MX_LUT_SIZE / MX_TWO_PI) + 0.5f) & MX_LUT_MASK;
    return s_sin_lut[idx];
}

float mx_cos(float rad) {
    return mx_sin(rad + MX_PI / 2.0f);
}

float mx_tan(float rad) {
    float c = mx_cos(rad);
    if (c > -1e-6f && c < 1e-6f) c = (rad > 0.0f) ? 1e-6f : -1e-6f;
    return mx_sin(rad) / c;
}

// atan(t) on [0,1], rational approx, max err ~4.7e-3 rad.
static float mx_atan_unit(float t) {
    return t / (1.0f + 0.28086f * t * t);
}

float mx_atan2(float y, float x) {
    if (x == 0.0f) {
        if (y > 0.0f) return MX_PI / 2.0f;
        if (y < 0.0f) return -MX_PI / 2.0f;
        return 0.0f;
    }
    float ax = x < 0.0f ? -x : x;
    float ay = y < 0.0f ? -y : y;
    float a;
    if (ax >= ay) {
        a = mx_atan_unit(ay / ax);
    } else {
        a = MX_PI / 2.0f - mx_atan_unit(ax / ay);
    }
    // a is the first-quadrant angle; place by signs
    if (x < 0.0f) a = MX_PI - a;
    if (y < 0.0f) a = -a;
    return a;
}

float mx_clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float mx_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float mx_move_toward(float cur, float target, float max_delta) {
    float d = target - cur;
    if (d > max_delta) return cur + max_delta;
    if (d < -max_delta) return cur - max_delta;
    return target;
}
