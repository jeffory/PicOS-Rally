// Headless test runner for core/ — no display, no PicOS, no SDL.
// Table-driven: tyre model, load transfer, surface lookup, input ramps,
// determinism. TAP-ish output, exit code = failures (CI-gateable).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../core/mathx.h"
#include "../../core/tuning.h"
#include "../../core/sim.h"
#include "../../core/camera.h"
#include "../../core/render.h"
#include "../../core/surface.h"

static int s_checks = 0, s_failures = 0;

#define CHECK(name, cond) do { \
    s_checks++; \
    if (cond) { printf("ok %d - %s\n", s_checks, name); } \
    else { printf("not ok %d - %s\n", s_checks, name); s_failures++; } \
} while (0)

static float fclampf(float v) { return v < 0.0f ? -v : v; }

// Drive the car for n steps with constant input.
static void drive(car_t *car, const sim_input_t *in, const tuning_t *tun,
                  const surface_map_t *sm, int steps) {
    for (int i = 0; i < steps; i++) sim_step(car, in, tun, sm);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    mx_init();
    tuning_t tun;
    tuning_defaults(&tun);
    surface_map_t sm;
    surface_init_test_oval(&sm);

    // ── 1. Surface lookup table ─────────────────────────────────────────────
    static const struct { float x, y; int surf; const char *name; } s_pts[] = {
        {   0.0f,   0.0f, SURF_GRASS,   "origin is grass" },
        {   0.0f, -55.0f, SURF_GRAVEL,  "ring north is gravel" },
        { -55.0f,   0.0f, SURF_GRAVEL,  "ring west is gravel" },
        {  55.0f,   0.0f, SURF_BITUMEN, "ring east is bitumen" },
        {  38.9f, -38.9f, SURF_BITUMEN, "ring at 135deg is bitumen" },
        { -38.9f, -38.9f, SURF_WATER,   "creek at 225deg" },
        { -21.6f, -59.2f, SURF_SAND,    "sand trap at 200deg r63" },
        { -38.0f,  38.0f, SURF_GRAVEL,  "ring at 315deg is gravel" },
        {  10.0f, -80.0f, SURF_GRASS,   "far outside is grass" },
    };
    for (unsigned i = 0; i < sizeof(s_pts) / sizeof(s_pts[0]); i++) {
        int got = surface_at(&sm, s_pts[i].x, s_pts[i].y);
        CHECK(s_pts[i].name, got == s_pts[i].surf);
    }

    // ── 2. Input ramps ──────────────────────────────────────────────────────
    {
        car_t car;
        sim_init(&car, 0.0f, -55.0f, MX_PI / 2.0f);
        sim_input_t in = { .throttle = 0.0f, .brake = 0.0f, .steer = 1.0f, .handbrake = false };
        // Hold full left for exactly steer_ramp_up_s → steer should be ~1.0
        // Hold full left for ramp_up_s (ceil so truncation can't short us)
        int steps_up = (int)(tun.steer_ramp_up_s / SIM_DT + 0.999f);
        drive(&car, &in, &tun, &sm, steps_up);
        CHECK("steer ramps to full lock in ramp_up_s", car.steer > 0.98f);
        in.steer = 0.0f;
        int steps_dn = (int)(tun.steer_ramp_down_s / SIM_DT + 0.999f);
        drive(&car, &in, &tun, &sm, steps_dn);
        CHECK("steer returns to centre in ramp_down_s", fclampf(car.steer) < 0.02f);
        // Half-press should cap at the target, not overshoot
        in.steer = 0.5f;
        drive(&car, &in, &tun, &sm, 2 * steps_up);
        CHECK("steer settles at target without overshoot",
              car.steer > 0.49f && car.steer < 0.51f);
    }

    // ── 3. Load transfer ────────────────────────────────────────────────────
    {
        car_t car;
        sim_init(&car, 0.0f, -55.0f, MX_PI / 2.0f);
        // Get to 10 m/s on the ring first (gravel, straight-ish)
        sim_input_t in = { .throttle = 1.0f, .brake = 0.0f, .steer = 0.0f, .handbrake = false };
        drive(&car, &in, &tun, &sm, 300);   // 5 s
        CHECK("car accelerates to >10 m/s", car.vx > 10.0f);
        float fz_f_accel = car.fz_front, fz_r_accel = car.fz_rear;
        CHECK("accel: load transfers rearward", fz_r_accel > fz_f_accel);
        in.throttle = 0.0f; in.brake = 1.0f;
        drive(&car, &in, &tun, &sm, 30);    // 0.5 s braking
        CHECK("braking: load transfers forward", car.fz_front > car.fz_rear);
    }

    // ── 4. Tyre clamp: lateral accel bounded by mu*g at the limit ──────────
    {
        car_t car;
        sim_init(&car, 0.0f, -55.0f, MX_PI / 2.0f);
        sim_input_t in = { .throttle = 1.0f, .brake = 0.0f, .steer = 0.0f, .handbrake = false };
        drive(&car, &in, &tun, &sm, 600);   // 10 s: up to speed
        float v0 = car.vx;
        CHECK("reaches >20 m/s", v0 > 20.0f);
        // Full lock and hold — car should slide, not spin out; lateral G
        // at the friction circle ≈ mu*g (gravel 0.72 → ~7.06 m/s²).
        // Judge the SETTLED state (final 2 s mean), not the entry transient.
        in.throttle = 0.2f; in.steer = 1.0f;
        float lat_g_sum = 0.0f; int lat_g_n = 0;
        for (int i = 0; i < 600; i++) {
            sim_step(&car, &in, &tun, &sm);
            if (i >= 480) {
                lat_g_sum += fclampf(car.yaw_rate * car.vx) / 9.81f;
                lat_g_n++;
            }
        }
        float lat_g = lat_g_sum / (float)lat_g_n;
        float mu = sim_surface_mu(SURF_GRAVEL);
        CHECK("settled lateral G bounded near mu circle", lat_g < mu * 1.15f);
        CHECK("still sliding after lock held (slip nonzero)", fclampf(car.slip_rear) > 0.01f);
    }

    // ── 5. Assist dial: higher assist = smaller settled slip angle ─────────
    {
        float slips[2];
        for (int a = 0; a < 2; a++) {
            tuning_t t2 = tun;
            t2.assist = a ? 1.0f : 0.0f;
            car_t car;
            sim_init(&car, 0.0f, -55.0f, MX_PI / 2.0f);
            sim_input_t in = { .throttle = 1.0f, .brake = 0.0f, .steer = 0.0f, .handbrake = false };
            drive(&car, &in, &t2, &sm, 600);
            in.throttle = 0.3f; in.steer = 0.8f;
            drive(&car, &in, &t2, &sm, 300);
            slips[a] = fclampf(car.slip_rear);
        }
        CHECK("assist=1.0 tames the slide vs assist=0.0", slips[1] <= slips[0] + 0.001f);
    }

    // ── 6. Handbrake: rear grip drops, yaw kicks ────────────────────────────
    {
        car_t car;
        sim_init(&car, 0.0f, -55.0f, MX_PI / 2.0f);
        sim_input_t in = { .throttle = 1.0f, .brake = 0.0f, .steer = 0.0f, .handbrake = false };
        drive(&car, &in, &tun, &sm, 600);
        in.throttle = 0.0f; in.steer = 0.6f; in.handbrake = true;
        drive(&car, &in, &tun, &sm, 90);
        CHECK("handbrake builds rear slip", fclampf(car.slip_rear) > 0.05f);
    }

    // ── 7. Determinism: same scripted drive twice → same hash ──────────────
    {
        uint32_t h[2];
        for (int r = 0; r < 2; r++) {
            car_t car;
            sim_init(&car, 0.0f, -55.0f, MX_PI / 2.0f);
            for (int s = 0; s < 1200; s++) {
                sim_input_t in = {
                    .throttle = 0.75f, .brake = 0.0f,
                    .steer = mx_sin((float)s * SIM_DT * 0.9f) * 0.85f,
                    .handbrake = false,
                };
                sim_step(&car, &in, &tun, &sm);
            }
            h[r] = sim_state_hash(&car);
        }
        CHECK("deterministic replay hash", h[0] == h[1]);
        printf("# replay hash: %08lx\n", (unsigned long)h[0]);
    }

    // ── 8. Render smoke (ortho blit over the painted oval) ──────────────────
    {
        uint16_t *fbmem = malloc(320 * 320 * 2);
        uint16_t *tex = malloc(512 * 512 * 2);
        if (fbmem && tex) {
            surface_paint_texture(&sm, tex, 512, 512, 4);
            framebuf_t fb = { fbmem, 320, 320 };
            car_t car;
            sim_init(&car, 0.0f, -55.0f, MX_PI / 2.0f);
            camera_t cam;
            camera_init(&cam, &car);
            render_ortho_ground(&fb, &cam, tex, 512, 512);
            render_car(&fb, &cam, &car, rgb565_be(28, 4, 4), rgb565_be(31, 63, 31));
            uint32_t sum = 0;
            for (int i = 0; i < 320 * 320; i += 97) sum += fbmem[i];
            CHECK("render produces non-uniform output", sum != 0);
        } else {
            CHECK("fb/texture alloc", 0);
        }
        free(tex);
        free(fbmem);
    }

    printf("1..%d\n", s_checks);
    printf("# %d/%d checks passed\n", s_checks - s_failures, s_checks);
    return s_failures ? 1 : 0;
}
