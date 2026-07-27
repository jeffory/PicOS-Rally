#include "sim.h"
#include "surface.h"
#include <string.h>

static const float s_surface_mu[SURF_COUNT] = {
    1.00f,  // bitumen
    0.72f,  // gravel
    0.55f,  // sand
    0.50f,  // grass
    0.42f,  // mud
    0.35f,  // water
};

float sim_surface_mu(int surface) {
    if (surface < 0 || surface >= SURF_COUNT) return s_surface_mu[SURF_GRAVEL];
    return s_surface_mu[surface];
}

void sim_init(car_t *car, float x, float y, float heading) {
    memset(car, 0, sizeof(*car));
    car->x = x; car->y = y; car->heading = heading;
}

void sim_step(car_t *car, const sim_input_t *in, const tuning_t *tun,
              const surface_src_t *src) {
    const float dt = SIM_DT;

    // ── Input ramps (digital steering IS the handling — §9) ─────────────────
    float steer_target = in->steer;
    float steer_rate = (steer_target * car->steer < 0.0f || steer_target == 0.0f)
                       ? 1.0f / tun->steer_ramp_down_s   // returning to centre
                       : 1.0f / tun->steer_ramp_up_s;    // winding on lock
    car->steer = mx_move_toward(car->steer, steer_target, steer_rate * dt);
    car->throttle = mx_move_toward(car->throttle, in->throttle,
                                   dt / tun->throttle_ramp_up_s);
    car->brake = mx_move_toward(car->brake, in->brake, dt / 0.060f);

    // Speed-dependent lock: linear drop to the knee, then constant high lock.
    float speed01 = mx_clamp(car->vx / tun->max_speed, 0.0f, 1.0f);
    float knee = tun->steer_curve_knee > 0.01f ? tun->steer_curve_knee : 0.01f;
    float curve_t = mx_clamp(speed01 / knee, 0.0f, 1.0f);
    float max_steer = mx_lerp(tun->steer_max_low_deg, tun->steer_max_high_deg,
                              curve_t) * (MX_PI / 180.0f);
    float delta = car->steer * max_steer;

    // ── Geometry + per-axle surface ─────────────────────────────────────────
    float lf = tun->wheelbase * tun->cg_front;         // CG to front axle
    float lr = tun->wheelbase - lf;                    // CG to rear axle
    int surf_f = SURF_GRAVEL, surf_r = SURF_GRAVEL;
    if (src) {
        float sh0 = mx_sin(car->heading), ch0 = mx_cos(car->heading);
        surf_f = src->at(src->ctx, car->x + sh0 * lf, car->y + ch0 * lf);
        surf_r = src->at(src->ctx, car->x - sh0 * lr, car->y - ch0 * lr);
    }
    float mu_f = sim_surface_mu(surf_f);
    float mu_r = sim_surface_mu(surf_r);
    float ca_f = tun->ca_front;
    float ca_r = tun->ca_rear;
    if (in->handbrake) {
        mu_r *= tun->hb_mu_cut;
        ca_r *= tun->hb_ca_cut;
    }
    float rr_scale = 0.5f * (surface_rr_scale(surf_f) + surface_rr_scale(surf_r));

    // ── Longitudinal force ──────────────────────────────────────────────────
    float v_fwd = car->vx < 0.0f ? 0.0f : car->vx;
    float engine = tun->engine_force * car->throttle
                 * (1.0f - v_fwd / tun->max_speed);
    if (engine < 0.0f) engine = 0.0f;
    float brake = tun->brake_force * car->brake;
    if (in->handbrake) brake += tun->brake_force * 0.5f;   // rear lock drag
    float drag = tun->drag * car->vx * (car->vx < 0.0f ? -car->vx : car->vx)
               + tun->rolling_res * rr_scale * car->vx;

    // ── Loads with longitudinal transfer ────────────────────────────────────
    float g = 9.81f;
    float ax_est = (engine - brake - drag) / tun->mass;    // for transfer
    float transfer = tun->mass * ax_est * tun->cg_height / tun->wheelbase;
    float fz_f = tun->mass * g * (lr / tun->wheelbase) - transfer;
    float fz_r = tun->mass * g * (lf / tun->wheelbase) + transfer;
    if (fz_f < 0.0f) fz_f = 0.0f;
    if (fz_r < 0.0f) fz_r = 0.0f;
    car->fz_front = fz_f; car->fz_rear = fz_r;

    // ── Traction limit: drive wheels can't put down more than mu*load ───────
    // THE surface loudness knob: gravel caps launches (wheelspin), water bogs.
    float traction = mu_r * fz_r;
    if (engine > traction) engine = traction;

    // ── Regime blend: kinematic below ~3 m/s ────────────────────────────────
    float spd = car->vx < 0.0f ? -car->vx : car->vx;
    float dyn = mx_clamp((spd - 1.5f) / 1.5f, 0.0f, 1.0f);  // 0@1.5 → 1@3 m/s

    float ax, ay;

    // Kinematic bicycle
    float kin_yaw_rate = car->vx / tun->wheelbase * mx_tan(delta);
    float kin_ax = ax_est;

    // Dynamic bicycle (guarded near zero speed)
    float vx_safe = car->vx;
    if (vx_safe > -0.5f && vx_safe < 0.5f) vx_safe = vx_safe < 0.0f ? -0.5f : 0.5f;
    float slip_f = mx_atan2(car->vy + car->yaw_rate * lf, vx_safe) - delta;
    float slip_r = mx_atan2(car->vy - car->yaw_rate * lr, vx_safe);
    car->slip_front = slip_f; car->slip_rear = slip_r;

    float fy_f = -ca_f * slip_f;
    float fy_r = -ca_r * slip_r;
    float fy_f_max = mu_f * fz_f;
    float fy_r_max = mu_r * fz_r;
    fy_f = mx_clamp(fy_f, -fy_f_max, fy_f_max);
    fy_r = mx_clamp(fy_r, -fy_r_max, fy_r_max);

    float fx = engine - drag;
    if (car->vx > 0.05f)       fx -= brake;
    else if (car->vx < -0.05f) fx += brake;          // brake opposes motion
    float dyn_ax = (fx - fy_f * mx_sin(delta)) / tun->mass + car->yaw_rate * car->vy;
    float dyn_ay = (fy_f * mx_cos(delta) + fy_r) / tun->mass - car->yaw_rate * car->vx;
    float dyn_yaw_acc = (lf * fy_f * mx_cos(delta) - lr * fy_r) / tun->iz;

    ax = mx_lerp(kin_ax, dyn_ax, dyn);
    ay = dyn_ay * dyn;
    // Yaw rate: kinematic at low speed, dynamic integration at speed.
    car->yaw_rate = mx_lerp(kin_yaw_rate, car->yaw_rate + dyn_yaw_acc * dt, dyn);

    // Handbrake yaw kick: with lock applied, bias rotation directly — the
    // arcade pivot that makes the handbrake useful for tight corners.
    if (in->handbrake && spd > 2.0f) {
        float s = car->steer < 0.0f ? -car->steer : car->steer;
        if (s > 0.3f)
            car->yaw_rate += (car->steer < 0.0f ? -1.0f : 1.0f) * tun->hb_yaw_kick * dt;
    }

    // Reverse creep: brake key acts as reverse when nearly stopped.
    if (car->vx < 0.5f && in->brake > 0.5f && car->throttle < 0.05f) {
        ax = -2.5f * in->brake;
    }

    // ── Assist: blend yaw toward velocity heading past a slip threshold ─────
    float slip_mag = car->slip_rear < 0.0f ? -car->slip_rear : car->slip_rear;
    if (slip_mag > tun->assist_slip && spd > 3.0f && tun->assist > 0.0f) {
        float vel_angle = mx_atan2(car->vy, vx_safe);
        // pull yaw_rate toward reducing the body-vs-velocity angle
        float corr = -vel_angle * tun->assist * tun->assist_rate;
        car->yaw_rate += corr * dt;
        // grip regains fast: damp lateral velocity growth
        car->vy *= 1.0f - tun->assist * 1.5f * dt;
    }

    // ── Integrate ───────────────────────────────────────────────────────────
    car->vx += ax * dt;
    car->vy += ay * dt;
    car->vy *= 1.0f - 1.2f * dt;   // tyre relaxation: ~2%/step lateral bleed
    if (car->vx > tun->max_speed) car->vx = tun->max_speed;
    if (car->vx < -8.0f) car->vx = -8.0f;

    car->heading += car->yaw_rate * dt;
    if (car->heading > MX_PI) car->heading -= MX_TWO_PI;
    if (car->heading < -MX_PI) car->heading += MX_TWO_PI;

    // world-frame displacement (heading: 0=+Y, clockwise)
    float sh = mx_sin(car->heading), ch = mx_cos(car->heading);
    float wx = sh * car->vx + ch * car->vy;
    float wy = ch * car->vx - sh * car->vy;
    car->x += wx * dt;
    car->y += wy * dt;

    car->accel_lon = ax;
    car->surface = surf_r;   // drive-axle surface (HUD/debug)
}

uint32_t sim_state_hash(const car_t *car) {
    const uint8_t *p = (const uint8_t *)car;
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < sizeof(car_t); i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}
