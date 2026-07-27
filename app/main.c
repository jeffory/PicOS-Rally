// PicOS Rally — M3: stage, race flow, pacenotes, splits.
// App layer: PicOS API glue only. Game logic lives in core/ (no PicOS headers).
//
// Drive: F5 throttle, F4 brake/reverse, LEFT/RIGHT steer, BACKSPACE handbrake.
// Flow: any drive key starts the countdown; F5 retries from results;
//       ESC/menu exits. F3 autopilot (AI drives), F9 debug overlay,
//       F2 pace 30/60/max, 'r' reloads tuning.
#include "app_abi.h"
#include "os.h"
#include <stdio.h>
#include <string.h>

#include "../core/mathx.h"
#include "../core/tuning.h"
#include "../core/sim.h"
#include "../core/camera.h"
#include "../core/render.h"
#include "../core/surface.h"
#include "../core/track.h"
#include "../core/ai.h"
#include "../core/render_track.h"

static const PicoCalcAPI *s_api;

static const char *const SURF_NAMES[SURF_COUNT] = {
    "BITUMEN", "GRAVEL", "SAND", "GRASS", "MUD", "WATER",
};

// ── Race states ─────────────────────────────────────────────────────────────
typedef enum {
    RS_INTRO, RS_COUNTDOWN, RS_RACING, RS_FINISH
} race_state_t;

#define MAX_CPS 32

typedef struct {
    race_state_t state;
    int countdown;              // frames left in countdown phase
    float stage_s;              // stage timer, seconds (sim-time, deterministic)
    float penalty_s;
    uint32_t best_total_ms;     // session best (0 = none; survives resets)
    // splits
    int next_cp;                // next checkpoint index
    float splits[MAX_CPS];      // stage_s at each checkpoint (0 = not yet)
    float last_split_s;         // most recent split for the HUD delta
    int last_split_idx;
    // off-course
    float offroad_t;            // seconds spent out of the road polygon
    // pacenotes
    const track_note_t *note;   // currently displayed note
    float note_t;               // seconds left to display
    // metrics
    float car_dist;             // arc distance along the line (m)
    float car_off;              // lateral offset from line (m)
} race_t;

static race_t s_race;
static track_t s_track;
static uint8_t *s_blob;
static ai_state_t s_ai;
static int s_autopilot = 0;
static surface_src_t s_tsrc;

// Surface palette (big-endian, indexed by SURF_*): matches the M2 oval look.
static uint16_t s_palette[8];

static void palette_init(void) {
    s_palette[SURF_BITUMEN] = rgb565_be(7, 8, 9);    // dark grey
    s_palette[SURF_GRAVEL]  = rgb565_be(13, 8, 5);   // red ochre
    s_palette[SURF_SAND]    = rgb565_be(22, 18, 9);  // tan
    s_palette[SURF_GRASS]   = rgb565_be(4, 13, 5);   // scrub green
    s_palette[SURF_MUD]     = rgb565_be(8, 6, 4);
    s_palette[SURF_WATER]   = rgb565_be(3, 14, 18);  // turquoise
    s_palette[6]            = rgb565_be(2, 6, 3);
    s_palette[7]            = rgb565_be(2, 6, 3);
}

// ── Input plumbing (poll wrapper: edges must be accumulated at EVERY poll) ──
static uint32_t s_pressed_accum;
static char s_char_accum;

static void app_poll(void) {
    s_api->sys->poll();
    uint32_t pressed = s_api->input->getButtonsPressed();
    s_pressed_accum |= pressed;
    char c = s_api->input->getChar();
    if (c) s_char_accum = c;
}

// ── Tuning reload ('r' / file on SD) ────────────────────────────────────────
static void reload_tuning(const PicoCalcAPI *api, const char *app_dir, tuning_t *tun) {
    char path[256];
    snprintf(path, sizeof(path), "%s/tuning/handling.toml", app_dir);
    pcfile_t f = api->fs->open(path, "rb");
    if (!f) return;
    static char buf[2048];
    int n = api->fs->read(f, buf, sizeof(buf) - 1);
    api->fs->close(f);
    if (n <= 0) return;
    buf[n] = 0;
    int unknown = 0;
    int applied = tuning_parse(tun, buf, n, &unknown);
    api->sys->log("RALLY: tuning: %d keys applied, %d unknown", applied, unknown);
}

// ── Stage load ──────────────────────────────────────────────────────────────
static int load_stage(const PicoCalcAPI *api, const char *app_dir) {
    char path[256];
    snprintf(path, sizeof(path), "%s/stage01.bin", app_dir);
    pcfile_t f = api->fs->open(path, "rb");
    if (!f) {
        api->sys->log("RALLY: no stage at %s", path);
        return 0;
    }
    int sz = api->fs->size(path);
    s_blob = api->psram->qmiAlloc((uint32_t)sz);
    if (!s_blob) {
        api->fs->close(f);
        api->sys->log("RALLY: stage blob alloc failed (%d)", sz);
        return 0;
    }
    int n = api->fs->read(f, s_blob, sz);
    api->fs->close(f);
    if (n != sz || !track_load(&s_track, s_blob, n)) {
        api->sys->log("RALLY: stage blob bad (%d/%d)", n, sz);
        api->psram->qmiFree(s_blob);
        s_blob = 0;
        return 0;
    }
    s_tsrc = track_surface_src(&s_track);
    api->sys->log("RALLY: stage loaded: %d pts, %d notes, %d cps, grid %dx%d",
                  s_track.num_points, s_track.num_notes, s_track.num_cps,
                  s_track.gw, s_track.gh);
    return 1;
}

// ── Race control ────────────────────────────────────────────────────────────
static void race_reset_run(car_t *car, camera_t *cam) {
    uint32_t best = s_race.best_total_ms;
    memset(&s_race, 0, sizeof(s_race));
    s_race.best_total_ms = best;
    s_race.state = RS_INTRO;
    float x, y, dx, dy, tv;
    track_line_at(&s_track, 0.0f, &x, &y, &dx, &dy, &tv);
    sim_init(car, x, y, mx_atan2(dx, dy));
    camera_init(cam, car);
    ai_init(&s_ai, &s_track);
}

// Called once per 60 Hz sim step during RS_RACING with the car's line metrics.
static void race_step(race_t *r, car_t *car, float dt) {
    float dist, halfw;
    float idx = track_closest(&s_track, car->x, car->y, &dist, &halfw);
    r->car_dist = idx * 2.0f;
    r->car_off = dist;

    r->stage_s += dt;

    // checkpoint splits
    if (r->next_cp < s_track.num_cps && r->car_dist >= s_track.cps[r->next_cp]) {
        r->splits[r->next_cp] = r->stage_s;
        r->last_split_s = r->stage_s;
        r->last_split_idx = r->next_cp;
        r->next_cp++;
    }

    // off-course: 3 s outside the polygon → 5 s penalty + respawn
    if (dist > halfw + 2.0f) {
        r->offroad_t += dt;
        if (r->offroad_t > 3.0f) {
            r->penalty_s += 5.0f;
            r->offroad_t = 0.0f;
            float x, y, dx, dy, tv;
            track_line_at(&s_track, idx, &x, &y, &dx, &dy, &tv);
            float sp = car->vx * 0.4f;
            sim_init(car, x, y, mx_atan2(dx, dy));
            car->vx = sp;
            s_api->sys->log("RALLY: off-course +5s (respawn at %.0fm)", idx * 2.0f);
        }
    } else {
        r->offroad_t = 0.0f;
    }

    // pacenotes: fire at ~2.5 s lead
    if (r->note_t > 0.0f) {
        r->note_t -= dt;
        if (r->note_t <= 0.0f) r->note = 0;
    }
    if (!r->note) {
        float lead = car->vx * 2.5f;
        if (lead < 40.0f) lead = 40.0f;
        const track_note_t *n = track_note_at(&s_track, r->car_dist + lead);
        if (n && n->dist - r->car_dist <= lead + 5.0f) {
            r->note = n;
            r->note_t = 3.0f;
        }
    }

    // finish
    if (r->car_dist >= s_track.length - 4.0f) {
        r->state = RS_FINISH;
        uint32_t total = (uint32_t)((r->stage_s + r->penalty_s) * 1000.0f);
        if (!r->best_total_ms || total < r->best_total_ms)
            r->best_total_ms = total;
    }
}

void picos_main(const PicoCalcAPI *api,
                const char *app_dir, const char *app_id, const char *app_name) {
    (void)app_id; (void)app_name;
    s_api = api;
    const picocalc_display_t *d = api->display;

    mx_init();
    palette_init();

    if (!load_stage(api, app_dir)) {
        d->clear(0);
        d->drawText(8, 8, "stage01.bin missing or bad", 0xF800, 0);
        d->drawText(8, 20, "run tools/trackbake.py bake", 0xFFFF, 0);
        d->flush();
        for (int i = 0; i < 300; i++) api->sys->poll();
        return;
    }

    tuning_t tun;
    tuning_defaults(&tun);
    reload_tuning(api, app_dir, &tun);

    car_t car;
    camera_t cam;
    race_reset_run(&car, &cam);

    framebuf_t fb = { d->getBackBuffer(), 320, 320 };

    int s_pace = 0;
    static const uint32_t s_pace_us[3] = { 33333, 16666, 0 };
    int s_debug = 1;
    uint32_t s_frames = 0;
    uint32_t acc_sim = 0, acc_render = 0, acc_present = 0;
    char line[96];

    api->sys->log("RALLY: M3 start (%s)", s_track.num_points > 0 ? "stage ok" : "no stage");

    uint64_t t_prev = api->sys->getTimeUs();
    uint32_t sim_acc_us = 0;

    for (;;) {
        if (api->sys->shouldExit()) break;

        // toggles
        uint32_t pressed = s_pressed_accum;
        s_pressed_accum = 0;
        if (pressed & BTN_F2) s_pace = (s_pace + 1) % 3;
        if (pressed & BTN_F3) { s_autopilot ^= 1;
            api->sys->log("RALLY: autopilot %d", s_autopilot); }
        if (pressed & BTN_F9) s_debug ^= 1;
        if (s_char_accum == 'r') reload_tuning(api, app_dir, &tun);
        if (s_char_accum == 'd') s_debug ^= 1;
        if (s_char_accum == 'p') { s_autopilot ^= 1;
            api->sys->log("RALLY: autopilot %d", s_autopilot); }
        if (s_char_accum == 27) break;
        s_char_accum = 0;

        // race flow transitions from input
        if (s_race.state == RS_INTRO) {
            if ((pressed & (BTN_F4 | BTN_F5 | BTN_ENTER)) || s_autopilot) {
                s_race.state = RS_COUNTDOWN;
                s_race.countdown = 3 * 60;   // 3 s at 60 Hz
            }
        } else if (s_race.state == RS_FINISH) {
            if (pressed & BTN_F5) race_reset_run(&car, &cam);
        }

        // ── Fixed-step sim (60 Hz) ──────────────────────────────────────────
        uint64_t t_now = api->sys->getTimeUs();
        sim_acc_us += (uint32_t)(t_now - t_prev);
        t_prev = t_now;
        int steps = 0;
        while (sim_acc_us >= 16667 && steps < 4) {
            app_poll();
            uint32_t b = api->input->getButtons();
            sim_input_t in;
            if (s_autopilot && s_race.state == RS_RACING) {
                ai_drive(&s_ai, &s_track, &car, &in);
            } else {
                in.throttle = (b & BTN_F5) ? 1.0f : 0.0f;
                in.brake = (b & BTN_F4) ? 1.0f : 0.0f;
                in.steer = 0.0f;
                if (b & BTN_LEFT) in.steer += 1.0f;
                if (b & BTN_RIGHT) in.steer -= 1.0f;
                in.handbrake = (b & BTN_BACKSPACE) != 0;
            }
            if (s_race.state == RS_COUNTDOWN) {
                // Hold the car on the line; just tick the counter. (The old
                // brake-hold triggered reverse creep — the car rolled off the
                // line during 3-2-1 and took a false penalty at GO.)
                car.vx = 0.0f; car.vy = 0.0f; car.yaw_rate = 0.0f;
                if (--s_race.countdown <= 0) s_race.state = RS_RACING;
            } else {
                uint64_t s0 = api->sys->getTimeUs();
                sim_step(&car, &in, &tun, &s_tsrc);
                acc_sim += (uint32_t)(api->sys->getTimeUs() - s0);
                if (s_race.state == RS_RACING)
                    race_step(&s_race, &car, SIM_DT);
            }
            sim_acc_us -= 16667;
            steps++;
        }
        if (steps == 4) sim_acc_us = 0;
        camera_update(&cam, &car, SIM_DT);

        // ── Render ──────────────────────────────────────────────────────────
        uint64_t r0 = api->sys->getTimeUs();
        fb.fb = d->getBackBuffer();
        if (s_race.state == RS_INTRO) {
            render_clear(&fb, s_palette[SURF_GRASS]);
            render_track_ground(&fb, &cam, &s_track, s_palette);
            render_track_line(&fb, &cam, &s_track, rgb565_be(31, 63, 31));
            render_car(&fb, &cam, &car, rgb565_be(28, 4, 4), rgb565_be(31, 63, 31));
            render_text(&fb, 68, 40, "COOLOOLA POINT", rgb565_be(31, 63, 31), 0);
            render_text(&fb, 62, 56, "2.7km gravel / shakedown", rgb565_be(24, 48, 24), 0);
            render_text(&fb, 88, 270, "F5 to start", rgb565_be(31, 63, 31), 0);
        } else {
            render_track_ground(&fb, &cam, &s_track, s_palette);
            render_track_line(&fb, &cam, &s_track, rgb565_be(20, 40, 20));
            render_car(&fb, &cam, &car, rgb565_be(28, 4, 4), rgb565_be(31, 63, 31));

            // HUD strip (bottom 40 px)
            render_fill_rect(&fb, 0, 280, 320, 40, rgb565_be(0, 0, 0));
            float tf = s_race.stage_s + s_race.penalty_s;
            uint32_t tms = (uint32_t)(tf * 1000.0f);
            snprintf(line, sizeof(line), "%lu:%02lu.%03lu",
                     (unsigned long)(tms / 60000),
                     (unsigned long)((tms % 60000) / 1000),
                     (unsigned long)(tms % 1000));
            render_text(&fb, 4, 284, line, rgb565_be(31, 63, 31), -1);
            snprintf(line, sizeof(line), "pen %lus CP %d/%d",
                     (unsigned long)s_race.penalty_s,
                     s_race.next_cp, s_track.num_cps);
            render_text(&fb, 130, 284, line, rgb565_be(24, 48, 24), -1);
            snprintf(line, sizeof(line), "%lddm %ldkm/h",
                     (long)(s_race.car_dist * 10.0f),
                     (long)(car.vx * 3.6f));
            render_text(&fb, 250, 284, line, rgb565_be(24, 48, 24), -1);
            // pacenote
            if (s_race.note) {
                render_text(&fb, 60, 296, s_race.note->text,
                            rgb565_be(31, 45, 10), -1);
            }
            if (s_race.state == RS_COUNTDOWN) {
                int n = (s_race.countdown / 60) + 1;
                snprintf(line, sizeof(line), "%d", n > 3 ? 3 : n);
                render_text(&fb, 152, 140, line, rgb565_be(31, 63, 31), 0);
            }
            if (s_race.state == RS_FINISH) {
                render_fill_rect(&fb, 40, 120, 240, 80, rgb565_be(0, 0, 0));
                uint32_t tot = (uint32_t)((s_race.stage_s + s_race.penalty_s) * 1000.0f);
                snprintf(line, sizeof(line), "FINISH %lu.%03lu",
                         (unsigned long)(tot / 1000), (unsigned long)(tot % 1000));
                render_text(&fb, 56, 132, line, rgb565_be(31, 63, 31), -1);
                snprintf(line, sizeof(line), "best %lu.%03lu",
                         (unsigned long)(s_race.best_total_ms / 1000),
                         (unsigned long)(s_race.best_total_ms % 1000));
                render_text(&fb, 56, 148, line, rgb565_be(24, 48, 24), -1);
                render_text(&fb, 56, 172, "F5 retry  ESC quit", rgb565_be(31, 63, 31), -1);
            }
            if (s_debug) {
                uint32_t fr = s_frames ? s_frames : 1;
                snprintf(line, sizeof(line), "sim %lu rnd %lu v %ldkm/h",
                         (unsigned long)(acc_sim / fr),
                         (unsigned long)(acc_render / fr),
                         (long)(car.vx * 3.6f));
                render_text(&fb, 4, 4, line, rgb565_be(31, 63, 31), 0);
                snprintf(line, sizeof(line), "slp %ld off %lddm %s",
                         (long)(car.slip_rear * 100.0f),
                         (long)(s_race.car_off * 10.0f),
                         SURF_NAMES[car.surface]);
                render_text(&fb, 4, 16, line, rgb565_be(31, 63, 31), 0);
            }
        }
        acc_render += (uint32_t)(api->sys->getTimeUs() - r0);

        // ── Present ─────────────────────────────────────────────────────────
        uint64_t p0 = api->sys->getTimeUs();
        d->flush();
        acc_present += (uint32_t)(api->sys->getTimeUs() - p0);
        s_frames++;

        if ((s_frames % 300) == 0) {
            api->sys->log("RALLY f%lu: sim=%luus render=%luus present=%luus",
                          (unsigned long)s_frames,
                          (unsigned long)(acc_sim / 300),
                          (unsigned long)(acc_render / 300),
                          (unsigned long)(acc_present / 300));
            acc_sim = acc_render = acc_present = 0;
            s_frames = 0;
        }

        // ── Pace ────────────────────────────────────────────────────────────
        uint32_t budget = s_pace_us[s_pace];
        if (budget) {
            uint64_t start = api->sys->getTimeUs();
            while (api->sys->getTimeUs() - start < budget)
                app_poll();
        }
    }

    api->sys->log("RALLY: exit");
    api->psram->qmiFree(s_blob);
}
