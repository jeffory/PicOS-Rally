// PicOS Rally — M4: real art, tile renderer, viewport/HUD split.
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
#include "../core/gfx.h"
#include "../core/tiles_sections.h"

static const PicoCalcAPI *s_api;

// palette indices into the CLUT (style.toml order)
enum {
    PAL_SCRUB_DEEP, PAL_GRASS_MID, PAL_GRASS_DRY, PAL_SPINIFEX, PAL_CANE, PAL_DUNE,
    PAL_GRAVEL_DEEP, PAL_GRAVEL_MID, PAL_GRAVEL_LIGHT, PAL_GRAVEL_DUST, PAL_RUT,
    PAL_SAND_WET, PAL_SAND_DRY, PAL_SAND_LIGHT, PAL_SAND_SHADOW,
    PAL_BIT_DEEP, PAL_BIT_MID, PAL_BIT_LIGHT, PAL_LINE_WHITE,
    PAL_WATER_DEEP, PAL_WATER_MID, PAL_WATER_LIGHT, PAL_WATER_FOAM, PAL_CREEK,
    PAL_MUD_DEEP, PAL_MUD_MID, PAL_MUD_LIGHT,
    PAL_CAR_WHITE, PAL_CAR_RED, PAL_CAR_BLUE, PAL_CAR_BLACK, PAL_CAR_GLASS, PAL_CAR_CHROME,
    PAL_GUM_BARK, PAL_GUM_LEAF, PAL_DEAD_WOOD, PAL_ROCK, PAL_SIGN_RED, PAL_SIGN_YELLOW,
    PAL_DUST, PAL_SHADOW, PAL_SKID, PAL_CREST,
    PAL_HUD_AMBER, PAL_HUD_AMBER_DIM, PAL_HUD_PANEL, PAL_HUD_PANEL_LIT, PAL_HUD_TEXT,
};

static const char *const SURF_NAMES[SURF_COUNT] = {
    "BITUMEN", "GRAVEL", "SAND", "GRASS", "MUD", "WATER",
};

#define VP_H 240   // viewport rows; HUD owns rows VP_H..319

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

// M4 gfx state: CLUT + tile sections + sprite sheets, all in PSRAM.
static gfx_t s_gfx;
static uint16_t *s_clut;              // 256 entries
static uint8_t *s_tile_secs[TILE_SEC_COUNT];
static uint8_t *s_car;                // 32 headings x 32x32 8bpp
static uint8_t *s_props;              // 24 x 32x32 8bpp
static uint8_t *s_hero;               // 64x64 8bpp intro art
#define CAR_SPRITE 32
#define PROP_SPRITE 32
#define HERO_SPRITE 64

// Legacy flat palette for the v1-blob fallback renderer only.
static uint16_t s_palette[8];

static void palette_init(void) {
    s_palette[SURF_BITUMEN] = rgb565_be(7, 8, 9);
    s_palette[SURF_GRAVEL]  = rgb565_be(13, 8, 5);
    s_palette[SURF_SAND]    = rgb565_be(22, 18, 9);
    s_palette[SURF_GRASS]   = rgb565_be(4, 13, 5);
    s_palette[SURF_MUD]     = rgb565_be(8, 6, 4);
    s_palette[SURF_WATER]   = rgb565_be(3, 14, 18);
    s_palette[6]            = rgb565_be(2, 6, 3);
    s_palette[7]            = rgb565_be(2, 6, 3);
}

// Read an asset file into a PSRAM allocation. Returns NULL on failure.
static void *load_bin(const PicoCalcAPI *api, const char *app_dir,
                      const char *name, int expect_sz) {
    char path[256];
    snprintf(path, sizeof(path), "%s/assets/%s", app_dir, name);
    pcfile_t f = api->fs->open(path, "rb");
    if (!f) {
        api->sys->log("RALLY: missing asset %s", name);
        return 0;
    }
    int sz = api->fs->size(path);
    if (expect_sz && sz != expect_sz) {
        api->sys->log("RALLY: asset %s size %d != %d", name, sz, expect_sz);
        api->fs->close(f);
        return 0;
    }
    void *buf = api->psram->qmiAlloc((uint32_t)sz);
    if (!buf) {
        api->sys->log("RALLY: asset %s alloc %d failed", name, sz);
        api->fs->close(f);
        return 0;
    }
    int n = api->fs->read(f, buf, sz);
    api->fs->close(f);
    if (n != sz) {
        api->sys->log("RALLY: asset %s read %d/%d", name, n, sz);
        api->psram->qmiFree(buf);
        return 0;
    }
    return buf;
}

static int load_assets(const PicoCalcAPI *api, const char *app_dir) {
    s_clut = load_bin(api, app_dir, "clut.bin", 512);
    if (!s_clut) return 0;
    for (int i = 0; i < TILE_SEC_COUNT; i++) {
        char name[64];
        snprintf(name, sizeof(name), "tiles_%s.bin", TILE_SEC_NAMES[i]);
        s_tile_secs[i] = load_bin(api, app_dir, name,
                                  TILE_SEC_SIZES[i] * 256);
        if (!s_tile_secs[i]) return 0;
    }
    gfx_init(&s_gfx, s_clut, (const uint8_t **)s_tile_secs,
             TILE_SEC_BASES, TILE_SEC_SIZES, TILE_SEC_COUNT);
    s_car = load_bin(api, app_dir, "car.bin", 32 * CAR_SPRITE * CAR_SPRITE);
    s_props = load_bin(api, app_dir, "props.bin", 24 * PROP_SPRITE * PROP_SPRITE);
    s_hero = load_bin(api, app_dir, "hero.bin", HERO_SPRITE * HERO_SPRITE);
    if (!s_car || !s_props || !s_hero) return 0;
    api->sys->log("RALLY: assets loaded (clut, %d tile secs, car, props, hero)",
                  TILE_SEC_COUNT);
    return 1;
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

// ── M4 render helpers ───────────────────────────────────────────────────────
static void render_car_sprite(uint16_t *fb, const camera_t *cam, const car_t *car) {
    float h = car->heading;
    while (h < 0.0f) h += 6.2831853f;
    while (h >= 6.2831853f) h -= 6.2831853f;
    int idx = (int)(h * (32.0f / 6.2831853f) + 0.5f) & 31;
    int sx = (int)((car->x - cam->x) * RENDER_PX_PER_M) + GFX_FB_W / 2 - CAR_SPRITE / 2;
    int sy = (int)((car->y - cam->y) * RENDER_PX_PER_M) + VP_H / 2 - CAR_SPRITE / 2;
    gfx_blit_sprite(&s_gfx, fb, VP_H, s_car + idx * CAR_SPRITE * CAR_SPRITE,
                    CAR_SPRITE, CAR_SPRITE, sx, sy);
}

static void render_hud(uint16_t *fb, const race_t *r, const car_t *car) {
    gfx_fill(&s_gfx, fb, 320, 0, VP_H, 320, 320 - VP_H, PAL_HUD_PANEL);
    gfx_fill(&s_gfx, fb, 320, 0, VP_H, 320, 1, PAL_HUD_AMBER);
    char line[64];
    // left: stage timer + penalty
    float tf = r->stage_s + r->penalty_s;
    uint32_t tms = (uint32_t)(tf * 1000.0f);
    snprintf(line, sizeof(line), "%lu:%02lu.%03lu",
             (unsigned long)(tms / 60000),
             (unsigned long)((tms % 60000) / 1000),
             (unsigned long)(tms % 1000));
    gfx_text(&s_gfx, fb, 320, 6, VP_H + 8, line, PAL_HUD_TEXT);
    snprintf(line, sizeof(line), "pen %lus", (unsigned long)r->penalty_s);
    gfx_text(&s_gfx, fb, 320, 6, VP_H + 20, line, PAL_HUD_AMBER_DIM);
    // centre: pacenote / split
    if (r->note) {
        gfx_text(&s_gfx, fb, 320, 110, VP_H + 8, r->note->text, PAL_HUD_AMBER);
    }
    if (r->last_split_idx >= 0) {
        snprintf(line, sizeof(line), "CP %d/%d", r->next_cp, s_track.num_cps);
        gfx_text(&s_gfx, fb, 320, 110, VP_H + 20, line, PAL_HUD_AMBER_DIM);
    }
    // right: speed + distance
    snprintf(line, sizeof(line), "%ld", (long)(car->vx * 3.6f));
    gfx_text(&s_gfx, fb, 320, 320 - 6 - gfx_text_width(line), VP_H + 8,
             line, PAL_HUD_TEXT);
    gfx_text(&s_gfx, fb, 320, 320 - 6 - 30, VP_H + 20, "km/h", PAL_HUD_AMBER_DIM);
    snprintf(line, sizeof(line), "%ldm", (long)r->car_dist);
    gfx_text(&s_gfx, fb, 320, 6, VP_H + 34, line, PAL_HUD_AMBER_DIM);
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
    if (!load_assets(api, app_dir)) {
        d->clear(0);
        d->drawText(8, 8, "assets missing (assets/*.bin)", 0xF800, 0);
        d->drawText(8, 20, "run the M4 bake tools", 0xFFFF, 0);
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
        int hud_dirty = 1;
        if (s_race.state == RS_INTRO) {
            render_track_ground_gfx(&s_gfx, fb.fb, &cam, &s_track, VP_H);
            render_track_props_gfx(&s_gfx, fb.fb, &cam, &s_track, VP_H,
                                   s_props, PROP_SPRITE);
            render_car_sprite(fb.fb, &cam, &car);
            // title plate: hero art + text over a dimmed band
            gfx_fill(&s_gfx, fb.fb, VP_H, 40, 90, 240, 96, PAL_HUD_PANEL);
            gfx_fill(&s_gfx, fb.fb, VP_H, 40, 90, 240, 2, PAL_HUD_AMBER);
            gfx_fill(&s_gfx, fb.fb, VP_H, 40, 184, 240, 2, PAL_HUD_AMBER);
            gfx_blit_sprite(&s_gfx, fb.fb, VP_H, s_hero, HERO_SPRITE, HERO_SPRITE,
                            52, 106);
            gfx_text(&s_gfx, fb.fb, VP_H, 128, 118, "COOLOOLA POINT", PAL_HUD_AMBER);
            gfx_text(&s_gfx, fb.fb, VP_H, 128, 134, "2.7km gravel / shakedown", PAL_HUD_TEXT);
            gfx_text(&s_gfx, fb.fb, VP_H, 128, 162, "F5 to start", PAL_HUD_AMBER);
        } else {
            render_track_ground_gfx(&s_gfx, fb.fb, &cam, &s_track, VP_H);
            render_track_props_gfx(&s_gfx, fb.fb, &cam, &s_track, VP_H,
                                   s_props, PROP_SPRITE);
            render_car_sprite(fb.fb, &cam, &car);

            if (s_race.state == RS_COUNTDOWN) {
                int n = (s_race.countdown / 60) + 1;
                snprintf(line, sizeof(line), "%d", n > 3 ? 3 : n);
                gfx_text(&s_gfx, fb.fb, VP_H, 156, 110, line, PAL_HUD_AMBER);
            }
            if (s_race.state == RS_FINISH) {
                gfx_fill(&s_gfx, fb.fb, VP_H, 40, 60, 240, 120, PAL_HUD_PANEL);
                gfx_fill(&s_gfx, fb.fb, VP_H, 40, 60, 240, 2, PAL_HUD_AMBER);
                uint32_t tot = (uint32_t)((s_race.stage_s + s_race.penalty_s) * 1000.0f);
                snprintf(line, sizeof(line), "FINISH %lu.%03lu",
                         (unsigned long)(tot / 1000), (unsigned long)(tot % 1000));
                gfx_text(&s_gfx, fb.fb, VP_H, 56, 76, line, PAL_HUD_AMBER);
                snprintf(line, sizeof(line), "best %lu.%03lu",
                         (unsigned long)(s_race.best_total_ms / 1000),
                         (unsigned long)(s_race.best_total_ms % 1000));
                gfx_text(&s_gfx, fb.fb, VP_H, 56, 92, line, PAL_HUD_TEXT);
                gfx_text(&s_gfx, fb.fb, VP_H, 56, 118, "F5 retry", PAL_HUD_AMBER);
                gfx_text(&s_gfx, fb.fb, VP_H, 56, 134, "ESC quit", PAL_HUD_TEXT);
                gfx_blit_sprite(&s_gfx, fb.fb, VP_H, s_hero, HERO_SPRITE, HERO_SPRITE,
                                200, 88);
            }
            if (s_debug) {
                uint32_t fr = s_frames ? s_frames : 1;
                snprintf(line, sizeof(line), "sim %lu rnd %lu v %ldkm/h",
                         (unsigned long)(acc_sim / fr),
                         (unsigned long)(acc_render / fr),
                         (long)(car.vx * 3.6f));
                gfx_text(&s_gfx, fb.fb, VP_H, 4, 4, line, PAL_HUD_TEXT);
                snprintf(line, sizeof(line), "slp %ld off %lddm %s",
                         (long)(car.slip_rear * 100.0f),
                         (long)(s_race.car_off * 10.0f),
                         SURF_NAMES[car.surface]);
                gfx_text(&s_gfx, fb.fb, VP_H, 4, 16, line, PAL_HUD_TEXT);
            }
        }
        render_hud(fb.fb, &s_race, &car);
        acc_render += (uint32_t)(api->sys->getTimeUs() - r0);

        // ── Present ─────────────────────────────────────────────────────────
        uint64_t p0 = api->sys->getTimeUs();
        d->flushRows(0, VP_H - 1);
        if (hud_dirty) d->flushRows(VP_H, 319);
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
    api->psram->qmiFree(s_clut);
    for (int i = 0; i < TILE_SEC_COUNT; i++) api->psram->qmiFree(s_tile_secs[i]);
    api->psram->qmiFree(s_car);
    api->psram->qmiFree(s_props);
    api->psram->qmiFree(s_hero);
}
