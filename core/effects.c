// effects — see header.
#include "effects.h"
#include "mathx.h"
#include "render.h"   // RENDER_PX_PER_M

// CLUT base indices (style.toml order — keep in sync with app/main.c)
#define PAL_SAND_LIGHT 13
#define PAL_GRASS_DRY   2
#define PAL_GRAVEL_DUST 9
#define PAL_MUD_LIGHT  26
#define PAL_WATER_FOAM 22
#define PAL_BIT_LIGHT  17
#define PAL_SKID_DARK  41
#define PAL_DUST       39

static const uint8_t SURF_DUST_PAL[6] = {
    PAL_BIT_LIGHT, PAL_GRAVEL_DUST, PAL_SAND_LIGHT,
    PAL_GRASS_DRY, PAL_MUD_LIGHT, PAL_WATER_FOAM,
};
// dust emission rate scale per surface
static const float SURF_DUST_RATE[6] = {
    0.15f, 1.0f, 0.9f, 0.5f, 0.7f, 1.4f,
};

void fx_init(fx_t *fx, uint32_t seed) {
    for (int i = 0; i < FX_MAX_PARTICLES; i++) fx->p[i].life = 0;
    fx->rng = seed ? seed : 0x9E3779B9u;
    fx->spawn_acc = 0.0f;
    fx->skid_head = 0;
    fx->skid_acc = 0.0f;
    fx->has_last = false;
}

static uint32_t fx_rand(fx_t *fx) {
    uint32_t l = fx->rng;
    l ^= l << 13; l ^= l >> 17; l ^= l << 5;
    fx->rng = l;
    return l;
}

// rear-axle world position
static void rear_axle(const car_t *car, float *rx, float *ry) {
    const float LR = 1.4f;   // matches sim's rear axle offset
    float sh = mx_sin(car->heading), ch = mx_cos(car->heading);
    *rx = car->x - sh * LR;
    *ry = car->y - ch * LR;
}

static void spawn(fx_t *fx, float x, float y, float vx, float vy,
                  uint8_t pal, uint8_t size, uint8_t life) {
    // overwrite the oldest/dead slot (ring by first-dead, else random)
    for (int i = 0; i < FX_MAX_PARTICLES; i++) {
        if (fx->p[i].life == 0) {
            fx_particle_t *p = &fx->p[i];
            p->x = x; p->y = y; p->vx = vx; p->vy = vy;
            p->pal = pal; p->size = size;
            p->life = p->life0 = life;
            return;
        }
    }
    int i = (int)(fx_rand(fx) & (FX_MAX_PARTICLES - 1));
    fx_particle_t *p = &fx->p[i];
    p->x = x; p->y = y; p->vx = vx; p->vy = vy;
    p->pal = pal; p->size = size;
    p->life = p->life0 = life;
}

void fx_step(fx_t *fx, const car_t *car) {
    int surf = car->surface;
    if (surf < 0 || surf > 5) surf = 3;

    // ── dust spawn: throttle + slip driven, speed-gated ──
    float slip = car->slip_rear < 0.0f ? -car->slip_rear : car->slip_rear;
    float rate = car->throttle * 0.6f + slip * 2.2f;
    if (car->vx < 1.5f) rate *= car->vx / 1.5f;
    rate *= SURF_DUST_RATE[surf];
    fx->spawn_acc += rate;
    float rx, ry;
    rear_axle(car, &rx, &ry);
    while (fx->spawn_acc >= 1.0f) {
        fx->spawn_acc -= 1.0f;
        float j1 = ((int)(fx_rand(fx) & 0xFF) - 128) * (1.0f / 128.0f);
        float j2 = ((int)(fx_rand(fx) & 0xFF) - 128) * (1.0f / 128.0f);
        float sh = mx_sin(car->heading), ch = mx_cos(car->heading);
        spawn(fx,
              rx + j1 * 0.7f, ry + j2 * 0.7f,
              -sh * car->vx * 0.25f + j1 * 1.6f,
              -ch * car->vx * 0.25f + j2 * 1.6f,
              SURF_DUST_PAL[surf],
              (uint8_t)(2 + (fx_rand(fx) & 1)),
              (uint8_t)(22 + (fx_rand(fx) & 15)));
    }

    // ── skids: lay a segment every 0.4 m of rear-axle travel while sliding ──
    if (slip > 0.10f && car->vx > 4.0f) {
        if (fx->has_last) {
            float dx = rx - fx->last_rx, dy = ry - fx->last_ry;
            float d2 = dx * dx + dy * dy;
            if (d2 > 0.16f) {   // 0.4 m
                fx_skid_seg_t *s = &fx->segs[fx->skid_head];
                s->x1_dm = (int16_t)(fx->last_rx * 10.0f);
                s->y1_dm = (int16_t)(fx->last_ry * 10.0f);
                s->x2_dm = (int16_t)(rx * 10.0f);
                s->y2_dm = (int16_t)(ry * 10.0f);
                fx->skid_head = (fx->skid_head + 1) & (FX_SKID_SEGS - 1);
                fx->last_rx = rx; fx->last_ry = ry;
            }
        } else {
            fx->last_rx = rx; fx->last_ry = ry;
            fx->has_last = true;
        }
    } else {
        fx->has_last = false;
    }

    // ── age particles ──
    for (int i = 0; i < FX_MAX_PARTICLES; i++) {
        fx_particle_t *p = &fx->p[i];
        if (!p->life) continue;
        p->x += p->vx * SIM_DT;
        p->y += p->vy * SIM_DT;
        p->vx *= 0.96f;
        p->vy *= 0.96f;
        p->life--;
    }
}

void fx_render_particles(const fx_t *fx, gfx_t *g, uint16_t *fb,
                         const camera_t *cam, int vp_h) {
    const int ppm = RENDER_PX_PER_M;
    for (int i = 0; i < FX_MAX_PARTICLES; i++) {
        const fx_particle_t *p = &fx->p[i];
        if (!p->life) continue;
        int sx = (int)((p->x - cam->x) * ppm + (GFX_FB_W / 2));
        int sy = (int)((p->y - cam->y) * ppm + (vp_h / 2));
        // fade: derived shade steps toward -20% as life runs out
        int shade = 48 + p->pal * 4 + ((p->life * 3) / (p->life0 ? p->life0 : 1));
        gfx_fill(g, fb, vp_h, sx - p->size / 2, sy - p->size / 2,
                 p->size, p->size, (uint8_t)shade);
    }
}

void fx_render_skids(const fx_t *fx, gfx_t *g, uint16_t *fb,
                     const camera_t *cam, int vp_h) {
    const int ppm = RENDER_PX_PER_M;
    for (int i = 0; i < FX_SKID_SEGS; i++) {
        const fx_skid_seg_t *s = &fx->segs[i];
        if (s->x1_dm == 0 && s->x2_dm == 0 && s->y1_dm == 0 && s->y2_dm == 0)
            continue;
        float x1 = s->x1_dm * 0.1f, y1 = s->y1_dm * 0.1f;
        float x2 = s->x2_dm * 0.1f, y2 = s->y2_dm * 0.1f;
        float dx = x2 - x1, dy = y2 - y1;
        float len2 = dx * dx + dy * dy;
        int steps = (int)(len2 * 2.0f) + 2;   // dot every ~0.7 m... cheap
        if (steps > 8) steps = 8;
        for (int k = 0; k <= steps; k++) {
            float t = (float)k / (float)steps;
            float wx = x1 + dx * t, wy = y1 + dy * t;
            int sx = (int)((wx - cam->x) * ppm + (GFX_FB_W / 2));
            int sy = (int)((wy - cam->y) * ppm + (vp_h / 2));
            gfx_fill(g, fb, vp_h, sx, sy, 2, 2, PAL_SKID_DARK);
        }
    }
}
