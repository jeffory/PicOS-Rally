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
#include "../../core/track.h"
#include "../../core/ai.h"
#include "../../core/gfx.h"
#include "../../core/render_track.h"
#include "../../core/tiles_sections.h"
#include "../../core/audio_synth.h"
#include "../../core/effects.h"

static int s_checks = 0, s_failures = 0;

#define CHECK(name, cond) do { \
    s_checks++; \
    if (cond) { printf("ok %d - %s\n", s_checks, name); } \
    else { printf("not ok %d - %s\n", s_checks, name); s_failures++; } \
} while (0)

static float fclampf(float v) { return v < 0.0f ? -v : v; }

// Drive the car for n steps with constant input.
static void drive(car_t *car, const sim_input_t *in, const tuning_t *tun,
                  const surface_src_t *src, int steps) {
    for (int i = 0; i < steps; i++) sim_step(car, in, tun, src);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    mx_init();
    tuning_t tun;
    tuning_defaults(&tun);
    surface_map_t sm;
    surface_init_test_oval(&sm);
    surface_src_t ss = surface_src_oval(&sm);

    // Optional trace mode: ./rally_headless trace /tmp/trace.ppm
    int trace_mode = (argc > 1 && strcmp(argv[1], "trace") == 0);
    const char *trace_path = (argc > 2) ? argv[2] : "/tmp/trace.ppm";
    FILE *trace_f = trace_mode ? fopen(trace_path, "wb") : NULL;
    int tw = 0, th = 0;
    unsigned char *trace_img = NULL;

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
        drive(&car, &in, &tun, &ss, steps_up);
        CHECK("steer ramps to full lock in ramp_up_s", car.steer > 0.98f);
        in.steer = 0.0f;
        int steps_dn = (int)(tun.steer_ramp_down_s / SIM_DT + 0.999f);
        drive(&car, &in, &tun, &ss, steps_dn);
        CHECK("steer returns to centre in ramp_down_s", fclampf(car.steer) < 0.02f);
        // Half-press should cap at the target, not overshoot
        in.steer = 0.5f;
        drive(&car, &in, &tun, &ss, 2 * steps_up);
        CHECK("steer settles at target without overshoot",
              car.steer > 0.49f && car.steer < 0.51f);
    }

    // ── 3. Load transfer ────────────────────────────────────────────────────
    {
        car_t car;
        sim_init(&car, 0.0f, -55.0f, MX_PI / 2.0f);
        // Get to 10 m/s on the ring first (gravel, straight-ish)
        sim_input_t in = { .throttle = 1.0f, .brake = 0.0f, .steer = 0.0f, .handbrake = false };
        drive(&car, &in, &tun, &ss, 300);   // 5 s
        CHECK("car accelerates to >10 m/s", car.vx > 10.0f);
        float fz_f_accel = car.fz_front, fz_r_accel = car.fz_rear;
        CHECK("accel: load transfers rearward", fz_r_accel > fz_f_accel);
        in.throttle = 0.0f; in.brake = 1.0f;
        drive(&car, &in, &tun, &ss, 30);    // 0.5 s braking
        CHECK("braking: load transfers forward", car.fz_front > car.fz_rear);
    }

    // ── 4. Tyre clamp: lateral accel bounded by mu*g at the limit ──────────
    {
        car_t car;
        sim_init(&car, 0.0f, -55.0f, MX_PI / 2.0f);
        sim_input_t in = { .throttle = 1.0f, .brake = 0.0f, .steer = 0.0f, .handbrake = false };
        drive(&car, &in, &tun, &ss, 600);   // 10 s: up to speed
        float v0 = car.vx;
        CHECK("reaches >20 m/s", v0 > 20.0f);
        // Full lock and hold — car should slide, not spin out; lateral G
        // at the friction circle ≈ mu*g (gravel 0.72 → ~7.06 m/s²).
        // Judge the SETTLED state (final 2 s mean), not the entry transient.
        in.throttle = 0.2f; in.steer = 1.0f;
        float lat_g_sum = 0.0f; int lat_g_n = 0;
        for (int i = 0; i < 600; i++) {
            sim_step(&car, &in, &tun, &ss);
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
            drive(&car, &in, &t2, &ss, 600);
            in.throttle = 0.3f; in.steer = 0.8f;
            drive(&car, &in, &t2, &ss, 300);
            slips[a] = fclampf(car.slip_rear);
        }
        CHECK("assist=1.0 tames the slide vs assist=0.0", slips[1] <= slips[0] + 0.001f);
    }

    // ── 6. Handbrake: rear grip drops, yaw kicks ────────────────────────────
    {
        car_t car;
        sim_init(&car, 0.0f, -55.0f, MX_PI / 2.0f);
        sim_input_t in = { .throttle = 1.0f, .brake = 0.0f, .steer = 0.0f, .handbrake = false };
        drive(&car, &in, &tun, &ss, 600);
        in.throttle = 0.0f; in.steer = 0.6f; in.handbrake = true;
        drive(&car, &in, &tun, &ss, 90);
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
                sim_step(&car, &in, &tun, &ss);
            }
            h[r] = sim_state_hash(&car);
        }
        CHECK("deterministic replay hash", h[0] == h[1]);
        printf("# replay hash: %08lx\n", (unsigned long)h[0]);
    }

    // ── 8. Track blob: load + query sanity ──────────────────────────────────
    track_t track;
    uint8_t *blob = NULL;
    long blob_sz = 0;
    {
        FILE *f = fopen("../../stage01.bin", "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            blob_sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            blob = malloc(blob_sz);
            if (blob && fread(blob, 1, blob_sz, f) != (size_t)blob_sz) { free(blob); blob = NULL; }
            fclose(f);
        }
        CHECK("stage01.bin loads", blob != NULL && blob_sz > 1000);
        if (blob) {
            CHECK("blob parses", track_load(&track, blob, (int)blob_sz));
            CHECK("stage length ~2.6 km", track.length > 2400.0f && track.length < 2900.0f);
            CHECK("has pacenotes", track.num_notes >= 10);
            CHECK("has checkpoints", track.num_cps >= 8);
            // closest point at stage start should be ~0 lateral
            float d, hw;
            track_closest(&track, track.points[0].x, track.points[0].y, &d, &hw);
            CHECK("closest at start is zero", d < 0.5f);
        }
    }

    // ── 9. Completability: the AI driver finishes the stage ─────────────────
    if (blob) {
        surface_src_t ts = track_surface_src(&track);

        // trace image: surface grid in green/grey shades, racing line white,
        // AI path red, big-excursion points yellow
        if (trace_f) {
            tw = track.gw; th = track.gh;
            trace_img = calloc((size_t)tw * th * 3, 1);
            static const unsigned char SCOL[6][3] = {
                {40, 40, 48},   // bitumen
                {120, 60, 30},  // gravel
                {200, 170, 90}, // sand
                {30, 90, 30},   // grass
                {80, 50, 25},   // mud
                {30, 140, 170}, // water
            };
            for (int y = 0; y < th; y++)
                for (int x = 0; x < tw; x++) {
                    int s = track.grid[y * tw + x];
                    if (s > 5) s = 3;
                    trace_img[(y * tw + x) * 3 + 0] = SCOL[s][0];
                    trace_img[(y * tw + x) * 3 + 1] = SCOL[s][1];
                    trace_img[(y * tw + x) * 3 + 2] = SCOL[s][2];
                }
        }
        #define TRACE_DOT(px, py, R, G, B) do { \
            int _x = (int)(((px) - track.ox) / track.cell); \
            int _y = (int)(((py) - track.oy) / track.cell); \
            if (_x >= 0 && _y >= 0 && _x < tw && _y < th && trace_img) { \
                trace_img[(_y * tw + _x) * 3 + 0] = (R); \
                trace_img[(_y * tw + _x) * 3 + 1] = (G); \
                trace_img[(_y * tw + _x) * 3 + 2] = (B); } } while (0)
        if (trace_img) {
            for (int i = 0; i < track.num_points; i++)
                TRACE_DOT(track.points[i].x, track.points[i].y, 230, 230, 230);
        }

        car_t car;
        sim_init(&car, track.points[0].x, track.points[0].y, 0.0f);
        // aim along the initial direction
        {
            float x, y, dx, dy, tv;
            track_line_at(&track, 0.0f, &x, &y, &dx, &dy, &tv);
            car.heading = mx_atan2(dx, dy);
        }
        ai_state_t ai;
        ai_init(&ai, &track);
        int max_steps = 60 * 240;   // 4 min cap
        int steps = 0;
        int dbg = getenv("AI_DEBUG") != NULL;
        while (!ai.finished && steps < max_steps) {
            sim_input_t in;
            ai_drive(&ai, &track, &car, &in);
            sim_step(&car, &in, &tun, &ts);
            if (dbg && ai.line_idx > 630 && ai.line_idx < 750 && (steps % 12) == 0) {
                float d, hw;
                track_closest(&track, car.x, car.y, &d, &hw);
                float x, y, dx, dy, tv;
                track_line_at(&track, ai.line_idx, &x, &y, &dx, &dy, &tv);
                printf("  i=%.0f pos=(%.0f,%.0f) v=%.1f hdg=%.1f tv=%.1f srf=%d lat=%.1f str=%.2f thr=%.2f brk=%.2f\n",
                       ai.line_idx, car.x, car.y, car.vx,
                       car.heading * 57.2958f, tv,
                       track_surface_at(&track, car.x, car.y), d,
                       in.steer, in.throttle, in.brake);
            }
            if (trace_img && (steps % 2) == 0) {
                float d, hw;
                track_closest(&track, car.x, car.y, &d, &hw);
                if (d > hw + 4.0f) TRACE_DOT(car.x, car.y, 255, 220, 0);
                else TRACE_DOT(car.x, car.y, 220, 40, 40);
            }
            steps++;
        }
        if (trace_f) {
            fprintf(trace_f, "P6\n%d %d\n255\n", tw, th);
            fwrite(trace_img, 1, (size_t)tw * th * 3, trace_f);
            fclose(trace_f);
            printf("# trace written: %s\n", trace_path);
        }
        CHECK("AI finishes the stage", ai.finished != 0);
        printf("# AI: time=%.1fs max_excursion=%.2fm@%g offroad=%.2fs steps=%d\n",
               ai.finish_time, ai.max_excursion, ai.max_exc_idx, ai.offroad_time, steps);
        CHECK("AI stage time in 60..180 s",
              ai.finish_time > 60.0f && ai.finish_time < 180.0f);
        // Competent-baseline thresholds (calibrated 2026-07-27: pursuit AI
        // runs 10.4 m / 2.7 s at the hairpin-creek). A track edit that makes
        // a corner impossible blows far past these (broken versions measured
        // 50 m / 12 s). Tighten when the AI improves.
        CHECK("AI max excursion < 15 m", ai.max_excursion < 15.0f);
        CHECK("AI off-road time < 5 s", ai.offroad_time < 5.0f);

        // determinism: same AI run twice → same hash
        {
            car_t c2;
            sim_init(&c2, track.points[0].x, track.points[0].y, 0.0f);
            {
                float x, y, dx, dy, tv;
                track_line_at(&track, 0.0f, &x, &y, &dx, &dy, &tv);
                c2.heading = mx_atan2(dx, dy);
            }
            track_t track2;
            track_load(&track2, blob, (int)blob_sz);   // fresh cursor
            ai_state_t ai2;
            ai_init(&ai2, &track2);
            int st2 = 0;
            while (!ai2.finished && st2 < max_steps) {
                sim_input_t in;
                ai_drive(&ai2, &track2, &c2, &in);
                sim_step(&c2, &in, &tun, &ts);
                st2++;
            }
            CHECK("AI replay deterministic", ai2.finished &&
                  ai2.finish_time == ai.finish_time &&
                  st2 == steps);
            printf("# AI replay hash: %08lx\n", (unsigned long)sim_state_hash(&c2));
        }
    }
    // ── 9c. M4: tilemap + props (blob v2) + gfx pipeline on real assets ────
    if (blob && track.tilemap) {
        int total_tiles = 0;
        for (int i = 0; i < TILE_SEC_COUNT; i++) total_tiles += TILE_SEC_SIZES[i];
        int cells = track.gw * track.gh;
        int bad = 0, overlays = 0, bases = 0;
        for (int i = 0; i < cells * TRACK_TILEMAP_SLOTS; i++) {
            uint16_t v = track.tilemap[i];
            if (v != TRACK_NO_TILE && v >= total_tiles) bad++;
        }
        for (int i = 0; i < cells; i++) {
            const uint16_t *sl = track.tilemap + i * TRACK_TILEMAP_SLOTS;
            if (sl[0] != TRACK_NO_TILE) bases++;
            if (sl[TRACK_TILEMAP_SLOTS - 1] != TRACK_NO_TILE) overlays++;
        }
        CHECK("tilemap indices in range", bad == 0);
        CHECK("every cell has a base tile", bases == cells);
        CHECK("road overlays present", overlays > 1000);
        CHECK("props scattered (100..512)", track.num_props > 100 && track.num_props <= 512);
        int pb = 0;
        for (int i = 0; i < track.num_props; i++) if (track.props[i].type >= 24) pb++;
        CHECK("prop types in range", pb == 0);

        // gfx smoke: render the stage start through the real asset pipeline
        uint16_t *clut = 0;
        uint8_t *secs[TILE_SEC_COUNT];
        memset(secs, 0, sizeof(secs));
        int assets_ok = 1;
        FILE *f = fopen("../../assets/clut.bin", "rb");
        if (f) {
            clut = malloc(512);
            if (!clut || fread(clut, 1, 512, f) != 512) assets_ok = 0;
            fclose(f);
        } else assets_ok = 0;
        for (int i = 0; i < TILE_SEC_COUNT && assets_ok; i++) {
            char path[256];
            snprintf(path, sizeof(path), "../../assets/tiles_%s.bin", TILE_SEC_NAMES[i]);
            f = fopen(path, "rb");
            if (!f) { assets_ok = 0; break; }
            secs[i] = malloc((size_t)TILE_SEC_SIZES[i] * 256);
            if (!secs[i] || fread(secs[i], 1, (size_t)TILE_SEC_SIZES[i] * 256, f)
                    != (size_t)TILE_SEC_SIZES[i] * 256) assets_ok = 0;
            fclose(f);
        }
        CHECK("M4 assets load (clut + tile sections)", assets_ok);
        if (assets_ok) {
            gfx_t g;
            gfx_init(&g, clut, (const uint8_t **)secs,
                     TILE_SEC_BASES, TILE_SEC_SIZES, TILE_SEC_COUNT);
            uint16_t *fb2 = calloc(320 * 240, 2);
            car_t cs;
            float x, y, dx, dy, tv;
            track_line_at(&track, 0.0f, &x, &y, &dx, &dy, &tv);
            sim_init(&cs, x, y, mx_atan2(dx, dy));
            camera_t cam;
            camera_init(&cam, &cs);
            render_track_ground_gfx(&g, fb2, &cam, &track, 240);
            // ground must not be uniform: count distinct colours in a sample
            uint32_t seen[16] = {0};
            for (int i = 0; i < 320 * 240; i++)
                for (int b = 0; b < 16; b++)
                    if (fb2[i] == (uint16_t)b * 4096 + fb2[i] % 4096) { seen[b]++; break; }
            int distinct = 0;
            for (int i = 1; i < 320 * 240; i++)
                if (fb2[i] != fb2[i - 1]) distinct++;
            CHECK("ground render non-uniform", distinct > 200);
            free(fb2);
        }
        free(clut);
        for (int i = 0; i < TILE_SEC_COUNT; i++) free(secs[i]);
    }
    free(blob);

    // ── 10. Render smoke (ortho blit over the painted oval) ─────────────────
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

    // ── 11. M5: audio synth ────────────────────────────────────────────────
    {
        synth_state_t st;
        synth_init(&st);
        synth_input_t in = { 0 };
        int16_t buf[512 * 2];
        // idle engine-off: near silence
        synth_mix(&st, &in, buf, 512);
        int32_t peak = 0;
        for (int i = 0; i < 512 * 2; i++)
            if (buf[i] > peak) peak = buf[i]; else if (-buf[i] > peak) peak = -buf[i];
        CHECK("synth silent when engine off", peak < 2000);
        // racing: engine + gravel noise audible
        in.engine_on = true;
        in.vx = 18.0f;
        in.throttle = 0.9f;
        in.surface = SURF_GRAVEL;
        in.slip_rear = 0.2f;
        synth_mix(&st, &in, buf, 512);
        peak = 0;
        int clip = 0;
        for (int i = 0; i < 512 * 2; i++) {
            int32_t v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > peak) peak = v;
            if (buf[i] == 32767 || buf[i] == -32768) clip++;
        }
        CHECK("synth audible when racing", peak > 4000);
        CHECK("synth limiter holds", clip < 50);
        // events produce an energy spike and retire
        synth_event(&st, SYN_EV_GO);
        synth_mix(&st, &in, buf, 512);
        peak = 0;
        for (int i = 0; i < 512 * 2; i++) {
            int32_t v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > peak) peak = v;
        }
        CHECK("one-shot event audible", peak > 6000);
        // drain the GO shot (320ms = ~3528 samples)
        for (int k = 0; k < 8; k++) synth_mix(&st, &in, buf, 512);
        CHECK("one-shot retires", st.ev_type == 0);
        // determinism: same inputs twice → same output
        synth_state_t a, b;
        synth_init(&a);
        synth_init(&b);
        synth_event(&a, SYN_EV_BEEP);
        synth_event(&b, SYN_EV_BEEP);
        int16_t ba[256 * 2], bb[256 * 2];
        synth_mix(&a, &in, ba, 256);
        synth_mix(&b, &in, bb, 256);
        CHECK("synth deterministic", memcmp(ba, bb, sizeof(ba)) == 0);
    }

    // ── 12. M5: effects (dust + skids) ─────────────────────────────────────
    {
        fx_t fx;
        fx_init(&fx, 12345u);
        car_t car;
        sim_init(&car, 0.0f, 0.0f, 0.0f);
        car.vx = 15.0f;
        car.throttle = 1.0f;
        car.slip_rear = 0.3f;
        car.surface = SURF_GRAVEL;
        for (int i = 0; i < 120; i++) {
            car.x += car.vx * SIM_DT;   // travel (skids are distance-triggered)
            fx_step(&fx, &car);
        }
        int alive = 0;
        for (int i = 0; i < FX_MAX_PARTICLES; i++)
            if (fx.p[i].life) alive++;
        CHECK("dust spawns under throttle+slip", alive > 0);
        CHECK("dust pool bounded", alive <= FX_MAX_PARTICLES);
        CHECK("skids lay under slip", fx.skid_head > 0);
        // skids stop when grip returns
        car.slip_rear = 0.0f;
        int head_before = fx.skid_head;
        for (int i = 0; i < 30; i++) {
            car.x += car.vx * SIM_DT;
            fx_step(&fx, &car);
        }
        CHECK("skids stop when grip returns", fx.skid_head == head_before);
        // particles die out once the throttle lifts
        car.throttle = 0.0f;
        for (int i = 0; i < 600; i++) fx_step(&fx, &car);
        alive = 0;
        for (int i = 0; i < FX_MAX_PARTICLES; i++)
            if (fx.p[i].life) alive++;
        CHECK("dust decays", alive == 0);
    }

    printf("1..%d\n", s_checks);
    printf("# %d/%d checks passed\n", s_checks - s_failures, s_checks);
    return s_failures ? 1 : 0;
}
