#include "tuning.h"
#include <string.h>
#include <stddef.h>

void tuning_defaults(tuning_t *t) {
    t->mass              = 1180.0f;
    t->wheelbase         = 2.55f;
    t->cg_front          = 0.52f;
    t->cg_height         = 0.55f;
    t->iz                = 1600.0f;
    t->max_speed         = 42.0f;
    t->engine_force      = 6000.0f;
    t->brake_force       = 9000.0f;
    t->drag              = 0.4257f;
    t->rolling_res       = 12.8f;
    t->ca_front          = 60000.0f;
    t->ca_rear           = 65000.0f;
    t->steer_ramp_up_s   = 0.100f;   // M2.3: 0.180 felt lazy — sharpened
    t->steer_ramp_down_s = 0.070f;   // M2.3: 0.110
    t->steer_max_low_deg  = 35.0f;
    t->steer_max_high_deg = 14.0f;   // M2.3: 12 felt like not enough lock at speed
    t->steer_curve_knee   = 0.60f;   // high lock reached at 60% of max speed
    t->throttle_ramp_up_s = 0.060f;  // M2.3: 0.090
    t->hb_yaw_kick        = 1.5f;
    t->hb_mu_cut          = 0.45f;   // M2.3: 0.55 was too subtle
    t->hb_ca_cut          = 0.40f;
    t->mu                = 0.72f;   // gravel — the stage surface
    t->assist            = 0.60f;
    t->assist_slip       = 0.22f;
    t->assist_rate       = 3.0f;
}

typedef struct { const char *key; float *field; } key_map_t;

static int apply_key(tuning_t *t, const char *key, int klen, float value) {
    const struct { const char *name; size_t off; } map[] = {
        {"mass",              offsetof(tuning_t, mass)},
        {"wheelbase",         offsetof(tuning_t, wheelbase)},
        {"cg_front",          offsetof(tuning_t, cg_front)},
        {"cg_height",         offsetof(tuning_t, cg_height)},
        {"iz",                offsetof(tuning_t, iz)},
        {"max_speed",         offsetof(tuning_t, max_speed)},
        {"engine_force",      offsetof(tuning_t, engine_force)},
        {"brake_force",       offsetof(tuning_t, brake_force)},
        {"drag",              offsetof(tuning_t, drag)},
        {"rolling_res",       offsetof(tuning_t, rolling_res)},
        {"ca_front",          offsetof(tuning_t, ca_front)},
        {"ca_rear",           offsetof(tuning_t, ca_rear)},
        {"steer.ramp_up_s",   offsetof(tuning_t, steer_ramp_up_s)},
        {"steer.ramp_down_s", offsetof(tuning_t, steer_ramp_down_s)},
        {"steer.max_low_deg", offsetof(tuning_t, steer_max_low_deg)},
        {"steer.max_high_deg",offsetof(tuning_t, steer_max_high_deg)},
        {"steer.curve_knee",  offsetof(tuning_t, steer_curve_knee)},
        {"throttle.ramp_up_s",offsetof(tuning_t, throttle_ramp_up_s)},
        {"hb.yaw_kick",       offsetof(tuning_t, hb_yaw_kick)},
        {"hb.mu_cut",         offsetof(tuning_t, hb_mu_cut)},
        {"hb.ca_cut",         offsetof(tuning_t, hb_ca_cut)},
        {"mu",                offsetof(tuning_t, mu)},
        {"assist",            offsetof(tuning_t, assist)},
        {"assist.slip",       offsetof(tuning_t, assist_slip)},
        {"assist.rate",       offsetof(tuning_t, assist_rate)},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        size_t n = strlen(map[i].name);
        if (n == (size_t)klen && memcmp(key, map[i].name, n) == 0) {
            *(float *)((char *)t + map[i].off) = value;
            return 1;
        }
    }
    return 0;
}

// strtod-lite: deterministic float parse (int.frac), no locale, no libm.
static float parse_float(const char *s, int len, int *used) {
    int i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    int neg = 0;
    if (i < len && (s[i] == '-' || s[i] == '+')) { neg = (s[i] == '-'); i++; }
    float whole = 0.0f;
    int digits = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        whole = whole * 10.0f + (float)(s[i] - '0');
        i++; digits++;
    }
    float frac = 0.0f, scale = 1.0f;
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            frac = frac * 10.0f + (float)(s[i] - '0');
            scale *= 10.0f;
            i++;
        }
    }
    if (used) *used = i;
    if (!digits) return 0.0f;
    float v = whole + frac / scale;
    return neg ? -v : v;
}

int tuning_parse(tuning_t *t, const char *buf, int len, int *unknown) {
    int applied = 0, unk = 0;
    int i = 0;
    while (i < len) {
        // line bounds
        int ls = i;
        while (i < len && buf[i] != '\n') i++;
        int le = i;                 // [ls, le) is the line
        if (i < len) i++;           // skip \n
        // strip comment
        for (int j = ls; j < le; j++) if (buf[j] == '#') { le = j; break; }
        // skip blanks / [sections]
        while (ls < le && (buf[ls] == ' ' || buf[ls] == '\t' || buf[ls] == '\r')) ls++;
        if (ls >= le || buf[ls] == '[') continue;
        // key
        int ks = ls;
        while (ls < le && buf[ls] != '=' && buf[ls] != ' ' && buf[ls] != '\t') ls++;
        int ke = ls;
        while (ls < le && (buf[ls] == ' ' || buf[ls] == '\t')) ls++;
        if (ls >= le || buf[ls] != '=') continue;
        ls++;
        while (ls < le && (buf[ls] == ' ' || buf[ls] == '\t')) ls++;
        int used = 0;
        float v = parse_float(buf + ls, le - ls, &used);
        if (used == 0) continue;
        if (apply_key(t, buf + ks, ke - ks, v)) applied++;
        else unk++;
    }
    if (unknown) *unknown = unk;
    return applied;
}
