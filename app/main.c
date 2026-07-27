// PicOS Rally — M1 grey box + Mode 7 spike.
// App layer: PicOS API glue only. Game logic lives in core/ (no PicOS headers).
//
// Keys:
//   UP/DOWN/LEFT/RIGHT  drive          SHIFT  handbrake
//   F1  projection ortho↔mode7         F2     pace 30/60/max
//   F3  debug overlay                  F4     autopilot
//   F5  surface cycle (mu)             F6     mode7 texture 256²-PSRAM↔64²-cachefit
//   F7/F8  mode7 cam_z −/+             F9     mode7 scale +
//   ESC (char 27) / system menu        exit
#include "app_abi.h"
#include "os.h"
#include <stdio.h>
#include <string.h>

#include "../core/mathx.h"
#include "../core/tuning.h"
#include "../core/sim.h"
#include "../core/camera.h"
#include "../core/render.h"

#define TEX256 256
#define TEX64  64

static const PicoCalcAPI *s_api;

// ── Ground textures (procedural grey-box stand-in for art) ─────────────────
// Host-order RGB565. 8 px checker + deterministic speckle + 64 px grid lines.
static uint16_t *s_tex256;          // 128 KB, PSRAM (thrash case) — host order
static uint16_t *s_tex64;           // 8 KB, PSRAM but XIP-cache-resident — host order
// NOTE (verified on hardware 2026-07-27 with a solid 0x07E0 probe): drawPlane
// takes HOST-order textures — its inner loop applies the same host→BE swap as
// every other firmware primitive. There is no special texture format.

#define RGB565H(r,g,b) ((uint16_t)(((r)&0x1F)<<11 | ((g)&0x3F)<<5 | ((b)&0x1F)))

static void gen_texture(uint16_t *tex, int n, uint32_t seed) {
    uint32_t s = seed ? seed : 1;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            s = s * 1664525u + 1013904223u;
            uint16_t c;
            int checker = ((x >> 3) ^ (y >> 3)) & 1;
            if ((x & 63) == 0 || (y & 63) == 0)
                c = RGB565H(10, 22, 10);                  // grid line (16 m)
            else if (checker)
                c = RGB565H(9, 18, 8);                    // dirt dark
            else
                c = RGB565H(11, 21, 9);                   // dirt light
            if ((s >> 29) == 0) c = RGB565H(16, 26, 12);  // speckle
            tex[y * n + x] = c;
        }
    }
}

// ── Mode 7 state ────────────────────────────────────────────────────────────
static int   s_projection = 0;      // 0 = ortho, 1 = mode7
static float s_m7_cam_z   = 24.0f;
static float s_m7_scale   = 8.0f;
static int   s_m7_horizon = 40;
static int   s_m7_texsel  = 0;      // 0 = 256², 1 = 64²

// ── Timing / pacing ─────────────────────────────────────────────────────────
static int s_pace = 0;              // 0 = 30fps, 1 = 60fps, 2 = max
static const uint32_t s_pace_us[3] = { 33333, 16666, 0 };

// ── Debug state ─────────────────────────────────────────────────────────────
static int s_debug = 1;
static int s_autopilot = 0;
static int s_surface = SURF_GRAVEL;

static uint32_t s_acc_sim_us, s_acc_render_us, s_acc_present_us, s_frames;
static float s_auto_t;
static uint32_t s_pressed_accum;
static char s_char_accum;

// Every sys->poll() call site must go through here: button edges live and die
// inside kbd_poll, so any poll that doesn't accumulate them eats presses
// (the busy-wait pace loop runs thousands of polls per frame).
static void app_poll(void) {
    s_api->sys->poll();
    uint32_t pressed = s_api->input->getButtonsPressed();
    s_pressed_accum |= pressed;
    char c = s_api->input->getChar();
    if (c) s_char_accum = c;
    // DIAG (M1 input debugging): log any held-state change or edge once.
    static uint32_t s_last_held = 0, s_last_edge_seen = 0;
    static char s_last_char_seen = 0;
    uint32_t held = s_api->input->getButtons();
    if (held != s_last_held) {
        s_api->sys->log("RALLY held: %08lx", (unsigned long)held);
        s_last_held = held;
    }
    if (pressed && pressed != s_last_edge_seen) {
        s_api->sys->log("RALLY edge: %08lx", (unsigned long)pressed);
        s_last_edge_seen = pressed;
    }
    if (c && c != s_last_char_seen) {
        s_api->sys->log("RALLY char: %d", (int)c);
        s_last_char_seen = c;
    }
}

void picos_main(const PicoCalcAPI *api,
                const char *app_dir, const char *app_id, const char *app_name) {
    (void)app_id; (void)app_name;
    s_api = api;
    const picocalc_display_t *d = api->display;

    mx_init();

    // ── Init allocations (everything up-front; no malloc after init) ────────
    s_tex256 = api->psram->qmiAlloc(TEX256 * TEX256 * 2);
    s_tex64  = api->psram->qmiAlloc(TEX64 * TEX64 * 2);
    if (!s_tex256 || !s_tex64) {
        api->sys->log("RALLY: texture alloc failed");
        return;
    }
    gen_texture(s_tex256, TEX256, 20260726u);
    gen_texture(s_tex64, TEX64, 77u);

    // ── Tuning: defaults + optional handling.toml override ──────────────────
    tuning_t tun;
    tuning_defaults(&tun);
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/tuning/handling.toml", app_dir);
        pcfile_t f = api->fs->open(path, "rb");
        if (f) {
            static char buf[2048];
            int n = api->fs->read(f, buf, sizeof(buf) - 1);
            api->fs->close(f);
            if (n > 0) {
                buf[n] = 0;
                int unknown = 0;
                int applied = tuning_parse(&tun, buf, n, &unknown);
                api->sys->log("RALLY: tuning %s: %d keys applied, %d unknown",
                              path, applied, unknown);
            }
        } else {
            api->sys->log("RALLY: no tuning file, defaults in use");
        }
    }

    car_t car;
    sim_init(&car, 0.0f, 0.0f, 0.0f);
    camera_t cam;
    camera_init(&cam, &car);

    framebuf_t fb = { d->getBackBuffer(), 320, 320 };

    api->sys->log("RALLY: grey box start (proj=%s, assist=%d)",
                  s_projection ? "mode7" : "ortho", (int)(tun.assist * 100));

    uint64_t t_prev = api->sys->getTimeUs();
    uint32_t sim_acc_us = 0;
    char line[96];

    for (;;) {
        // shouldExit() CONSUMES the flag — exactly one call site may check it,
        // here at frame top. (A second check in the pace loop used to swallow
        // the flag there 99% of the time and the app could never exit.)
        if (api->sys->shouldExit()) break;

        // ── Toggles read pressed_accum, filled by the sim-step polls below ──
        uint32_t pressed = s_pressed_accum;
        s_pressed_accum = 0;
        if (pressed & BTN_F1) {
            s_projection ^= 1;
            api->sys->log("RALLY: projection -> %s", s_projection ? "mode7" : "ortho");
        }
        if (pressed & BTN_F2) s_pace = (s_pace + 1) % 3;
        if (pressed & BTN_F3) s_debug ^= 1;
        if (pressed & BTN_F4) s_autopilot ^= 1;
        if (pressed & BTN_F5) {
            s_surface = (s_surface + 1) % SURF_COUNT;
            api->sys->log("RALLY: surface=%d mu=%d", s_surface,
                          (int)(sim_surface_mu(s_surface) * 100));
        }
        if (pressed & BTN_F6) {
            s_m7_texsel ^= 1;
            api->sys->log("RALLY: mode7 tex -> %s", s_m7_texsel ? "64sq-cachefit" : "256sq-psram");
        }
        if (pressed & BTN_F7) { s_m7_cam_z -= 4.0f; if (s_m7_cam_z < 4.0f) s_m7_cam_z = 4.0f;
            api->sys->log("RALLY: cam_z=%d scale=%d", (int)s_m7_cam_z, (int)s_m7_scale); }
        if (pressed & BTN_F8) { s_m7_cam_z += 4.0f; if (s_m7_cam_z > 64.0f) s_m7_cam_z = 64.0f;
            api->sys->log("RALLY: cam_z=%d scale=%d", (int)s_m7_cam_z, (int)s_m7_scale); }
        if (pressed & BTN_F9) { s_m7_scale += 1.0f; if (s_m7_scale > 32.0f) s_m7_scale = 1.0f;
            api->sys->log("RALLY: cam_z=%d scale=%d", (int)s_m7_cam_z, (int)s_m7_scale); }
        if (pressed & BTN_TAB) {
            s_m7_horizon = (s_m7_horizon == 40) ? 100 : (s_m7_horizon == 100) ? 160 : 40;
            api->sys->log("RALLY: horizon=%d", s_m7_horizon);
        }
        // Char-key mirrors of the F-key toggles — the serial injection path
        // delivers chars far more reliably than button bits, so these are the
        // primary remote-control channel on hardware.
        switch (s_char_accum) {
        case 'm': s_projection ^= 1;
            api->sys->log("RALLY: projection -> %s", s_projection ? "mode7" : "ortho"); break;
        case 'p': s_autopilot ^= 1;
            api->sys->log("RALLY: autopilot %d", s_autopilot); break;
        case 'd': s_debug ^= 1; break;
        case 'g': s_surface = (s_surface + 1) % SURF_COUNT;
            api->sys->log("RALLY: surface=%d", s_surface); break;
        case 't': s_m7_texsel ^= 1;
            api->sys->log("RALLY: tex=%d", s_m7_texsel); break;
        case '-': s_m7_cam_z -= 4.0f; if (s_m7_cam_z < 4.0f) s_m7_cam_z = 4.0f;
            api->sys->log("RALLY: cam_z=%d", (int)s_m7_cam_z); break;
        case '=': s_m7_cam_z += 4.0f; if (s_m7_cam_z > 64.0f) s_m7_cam_z = 64.0f;
            api->sys->log("RALLY: cam_z=%d", (int)s_m7_cam_z); break;
        case '[': s_m7_scale -= 1.0f; if (s_m7_scale < 1.0f) s_m7_scale = 32.0f;
            api->sys->log("RALLY: scale=%d", (int)s_m7_scale); break;
        case ']': s_m7_scale += 1.0f; if (s_m7_scale > 32.0f) s_m7_scale = 1.0f;
            api->sys->log("RALLY: scale=%d", (int)s_m7_scale); break;
        case 'h': s_m7_horizon = (s_m7_horizon == 40) ? 100 : (s_m7_horizon == 100) ? 160 : 40;
            api->sys->log("RALLY: horizon=%d", s_m7_horizon); break;
        case '0': case '1': case '2': s_pace = s_char_accum - '0';
            api->sys->log("RALLY: pace=%d", s_pace); break;
        default: break;
        }
        if (s_char_accum == 27) break;
        s_char_accum = 0;

        // ── Fixed-step sim (60 Hz); input re-sampled per step, not per frame.
        // Button edges are accumulated per poll() — reading getButtonsPressed
        // once per frame races the per-step polls and loses presses. ─────────
        uint64_t t_now = api->sys->getTimeUs();
        sim_acc_us += (uint32_t)(t_now - t_prev);
        t_prev = t_now;
        int steps = 0;
        while (sim_acc_us >= 16667 && steps < 4) {
            app_poll();
            uint32_t b = api->input->getButtons();
            sim_input_t in;
            if (s_autopilot) {
                in.throttle = 0.75f;
                in.brake = 0.0f;
                s_auto_t += SIM_DT;
                in.steer = mx_sin(s_auto_t * 0.9f) * 0.85f;
                in.handbrake = false;
            } else {
                in.throttle = (b & BTN_UP) ? 1.0f : 0.0f;
                in.brake = (b & BTN_DOWN) ? 1.0f : 0.0f;
                in.steer = 0.0f;
                if (b & BTN_LEFT) in.steer += 1.0f;
                if (b & BTN_RIGHT) in.steer -= 1.0f;
                in.handbrake = (b & BTN_SHIFT) != 0;
            }
            uint64_t s0 = api->sys->getTimeUs();
            sim_step(&car, &in, &tun, s_surface);
            s_acc_sim_us += (uint32_t)(api->sys->getTimeUs() - s0);
            sim_acc_us -= 16667;
            steps++;
        }
        if (steps == 4) sim_acc_us = 0;   // spiral-of-death guard
        camera_update(&cam, &car, SIM_DT);

        // ── Render ──────────────────────────────────────────────────────────
        uint64_t r0 = api->sys->getTimeUs();
        fb.fb = d->getBackBuffer();
        if (s_projection == 0) {
            const uint16_t *tex = s_m7_texsel ? s_tex64 : s_tex256;
            int tn = s_m7_texsel ? TEX64 : TEX256;
            render_ortho_ground(&fb, &cam, tex, tn, tn);
            render_car(&fb, &cam, &car, rgb565_be(28, 4, 4), rgb565_be(31, 63, 31));
        } else {
            // drawPlane wants HOST-order textures (its inner loop byte-swaps
            // into the big-endian back buffer, exactly like the other
            // firmware primitives). The earlier BE assumption was wrong.
            const uint16_t *tex = s_m7_texsel ? s_tex64 : s_tex256;
            int tn = s_m7_texsel ? TEX64 : TEX256;
            // Clear the void above the horizon (drawPlane only fills below it).
            // API primitives take HOST-order colours (drawPlane textures are
            // the odd ones out — they want big-endian; see gen_texture note).
            d->fillRect(0, 0, 320, s_m7_horizon + 1, RGB565H(1, 3, 4));
            d->drawPlane(tex, tn, tn,
                         car.x * RENDER_PX_PER_M, car.y * RENDER_PX_PER_M,
                         s_m7_cam_z, car.heading, s_m7_horizon, s_m7_scale);
            // chase-cam car placeholder at bottom centre
            uint16_t body = RGB565H(28, 4, 4), edge = RGB565H(31, 63, 31);
            d->fillRect(160 - 6, 250 - 9, 12, 18, body);
            d->drawRect(160 - 6, 250 - 9, 12, 18, edge);
        }
        // ── Debug overlay ───────────────────────────────────────────────────
        // Buffer-authority rule (sim compositing): ortho mode is fully direct
        // (render_* into the back buffer); mode7 mode is fully API (drawPlane
        // writes the same buffer firmware-side). Overlay text follows the mode.
        if (s_debug) {
            uint32_t fr = s_frames ? s_frames : 1;
            uint16_t fg = rgb565_be(31, 63, 31);       // direct path: big-endian
            uint16_t fg_h = RGB565H(31, 63, 31);       // API path: host order
            snprintf(line, sizeof(line), "sim %lu rnd %lu dma %lu",
                     (unsigned long)(s_acc_sim_us / fr),
                     (unsigned long)(s_acc_render_us / fr),
                     (unsigned long)(s_acc_present_us / fr));
            if (s_projection == 0) render_text(&fb, 4, 4, line, fg, 0);
            else d->drawText(4, 4, line, fg_h, 0);
            snprintf(line, sizeof(line), "v %ldkm/h slipR %ld mu %ld ast %ld %s",
                     (long)(car.vx * 3.6f),
                     (long)(car.slip_rear * 100.0f),
                     (long)(sim_surface_mu(s_surface) * 100.0f),
                     (long)(tun.assist * 100.0f),
                     s_projection ? "M7" : "ORT");
            if (s_projection == 0) render_text(&fb, 4, 16, line, fg, 0);
            else d->drawText(4, 16, line, fg_h, 0);
            if (s_projection) {
                snprintf(line, sizeof(line), "cz %ld sc %ld hz %d tx %d",
                         (long)s_m7_cam_z, (long)s_m7_scale,
                         s_m7_horizon, s_m7_texsel);
                d->drawText(4, 28, line, fg_h, 0);
            }
        }
        s_acc_render_us += (uint32_t)(api->sys->getTimeUs() - r0);

        // ── Present ─────────────────────────────────────────────────────────
        uint64_t p0 = api->sys->getTimeUs();
        d->flush();
        s_acc_present_us += (uint32_t)(api->sys->getTimeUs() - p0);
        s_frames++;

        // ── Bench log every 300 frames ──────────────────────────────────────
        if ((s_frames % 300) == 0) {
            uint32_t fr = 300;
            api->sys->log("RALLY f%lu: sim=%luus render=%luus present=%luus",
                          (unsigned long)s_frames,
                          (unsigned long)(s_acc_sim_us / fr),
                          (unsigned long)(s_acc_render_us / fr),
                          (unsigned long)(s_acc_present_us / fr));
            api->sys->log("RALLY ctx: proj=%s tex=%d cz=%ld sc=%ld",
                          s_projection ? "M7" : "ORT", s_m7_texsel,
                          (long)s_m7_cam_z, (long)s_m7_scale);
            s_acc_sim_us = s_acc_render_us = s_acc_present_us = 0;
            s_frames = 0;
        }

        // ── Pace ────────────────────────────────────────────────────────────
        uint32_t budget = s_pace_us[s_pace];
        if (budget) {
            uint64_t start = api->sys->getTimeUs();
            while (api->sys->getTimeUs() - start < budget) {
                app_poll();
            }
        }
    }

    api->sys->log("RALLY: exit");
    api->psram->qmiFree(s_tex256);
    api->psram->qmiFree(s_tex64);
}
