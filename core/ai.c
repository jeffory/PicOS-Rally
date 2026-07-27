#include "ai.h"

// Pure pursuit: aim at a lookahead point on the racing line; curvature
// feedforward sets the base lock, heading error corrects. Speed follows the
// baked target with a brake margin. No handbrake (keeps the line honest).

void ai_init(ai_state_t *ai, const track_t *t) {
    (void)t;
    ai->line_idx = 0.0f;
    ai->max_excursion = 0.0f;
    ai->offroad_time = 0.0f;
    ai->finished = 0;
    ai->finish_time = 0.0f;
}

void ai_drive(ai_state_t *ai, const track_t *t, const car_t *car,
              sim_input_t *in) {
    // Where are we on the line? (track cursor makes this O(1) amortized)
    float dist, halfw;
    track_t *tm = (track_t *)t;   // cursor lives in the struct
    float closest = track_closest(tm, car->x, car->y, &dist, &halfw);

    // Monotonic clamp: when the car overshoots a tight corner, the nearest
    // line point can be the RETURN leg of the same hairpin — a backwards
    // jump that teleports the lookahead and cuts the corner. Only allow
    // small backwards steps; big ones mean "off course, keep your place".
    if (closest < ai->line_idx - 4.0f) {
        closest = ai->line_idx;
    } else {
        ai->line_idx = closest;
    }

    float adist = dist < 0.0f ? -dist : dist;
    if (adist > ai->max_excursion) {
        ai->max_excursion = adist;
        ai->max_exc_idx = ai->line_idx;
    }
    if (adist > halfw + 1.0f) ai->offroad_time += SIM_DT;

    // Lookahead: ~0.8 s ahead, clamped to 6..20 m (indices are 2 m each).
    // Shrinks in tight corners (low target speed) so the pursuit doesn't cut.
    float speed = car->vx < 0.0f ? 0.0f : car->vx;
    float look_m = 6.0f + speed * 0.8f;
    if (look_m > 20.0f) look_m = 20.0f;
    // Peek the target speed ahead to size lookahead + recovery
    float peek_idx = ai->line_idx + look_m * 0.5f;
    if (peek_idx > t->num_points - 1.0f) peek_idx = t->num_points - 1.0f;
    float lx, ly, dx, dy, target_v;
    track_line_at(t, peek_idx, &lx, &ly, &dx, &dy, &target_v);
    if (target_v < 10.0f) {
        float tight_look = 4.0f + target_v * 0.8f;
        if (tight_look < look_m) {
            look_m = tight_look;
            peek_idx = ai->line_idx + look_m * 0.5f;
            if (peek_idx > t->num_points - 1.0f) peek_idx = t->num_points - 1.0f;
            track_line_at(t, peek_idx, &lx, &ly, &dx, &dy, &target_v);
        }
    }
    // Off-course recovery (early: beyond road edge + 1 m): close in on the
    // rejoin point with limited throttle; if the point is far across the
    // infield, brake down to walking pace instead of plowing deeper.
    int recovering = adist > halfw + 1.0f;
    if (recovering) {
        track_line_at(t, ai->line_idx + 3.0f, &lx, &ly, &dx, &dy, &target_v);
    }

    // Low-speed / water recovery: the pursuit degenerates when nearly stopped
    // (or bogged) — point at a near forward point and just drive out.
    int surf_here = track_surface_at(t, car->x, car->y);
    int low_speed = (car->vx < 2.5f && car->vx > -0.5f);
    if (low_speed || surf_here == SURF_WATER) {
        float fx, fy, fdx, fdy, ftv;
        track_line_at(t, ai->line_idx + 3.0f, &fx, &fy, &fdx, &fdy, &ftv);
        float sh2 = mx_sin(car->heading), ch2 = mx_cos(car->heading);
        float rx = fx - car->x, ry = fy - car->y;
        float fwd = rx * sh2 + ry * ch2;
        float lat = rx * ch2 - ry * sh2;
        float ang = mx_atan2(lat, fwd > 0.5f ? fwd : 0.5f);
        in->steer = mx_clamp(ang * 1.2f - car->yaw_rate * 0.5f, -1.0f, 1.0f);
        in->throttle = surf_here == SURF_WATER ? 0.7f : 0.85f;
        in->brake = 0.0f;
        in->handbrake = false;
        if (!ai->finished) {
            ai->finish_time += SIM_DT;
            if (ai->line_idx >= (float)t->num_points - 2.0f)
                ai->finished = 1;
        }
        return;
    }

    // Steering: heading angle to lookahead point, in car frame. Pursuit gain
    // plus countersteer damping against rear slip (kills the weave) and
    // explicit anti-spin control when yaw runs away.
    float sh = mx_sin(car->heading), ch = mx_cos(car->heading);
    float rx = lx - car->x, ry = ly - car->y;
    // forward and left components
    float fwd = rx * sh + ry * ch;
    float lat = rx * ch - ry * sh;
    float ang = mx_atan2(lat, fwd > 0.5f ? fwd : 0.5f);
    float steer = ang * 1.3f - car->slip_rear * 0.8f - car->yaw_rate * 0.35f;
    steer = mx_clamp(steer, -1.0f, 1.0f);

    // Speed: follow the baked target with a safety margin (0.85×) — the bake
    // is the physical limit, the AI should sit below it.
    target_v *= 0.85f;
    float v_err = target_v - speed;
    float throttle = 0.0f, brake = 0.0f;
    if (target_v < 9.0f) {
        // tight zone: arrive under target, exit gently
        if (v_err > 1.5f) throttle = 0.6f;
        else if (v_err < -0.3f) brake = 0.6f;
    } else {
        if (v_err > 0.5f) throttle = v_err > 4.0f ? 1.0f : 0.6f;
        else if (v_err < -1.0f) brake = v_err < -4.0f ? 1.0f : 0.5f;
    }
    // Traction control: big rear slip + throttle = wheelspin slide. Back off.
    float slip_mag = car->slip_rear < 0.0f ? -car->slip_rear : car->slip_rear;
    if (slip_mag > 0.18f && throttle > 0.2f)
        throttle *= 0.5f;
    // Anti-spin: yaw running away → cut power entirely.
    float yaw_mag = car->yaw_rate < 0.0f ? -car->yaw_rate : car->yaw_rate;
    if (yaw_mag > 1.8f) throttle = 0.0f;
    if (recovering) {
        if (throttle > 0.4f) throttle = 0.4f;
        if (target_v > 12.0f) target_v = 12.0f;
        // Sliding sideways in recovery: cut power entirely, straighten out.
        if (slip_mag > 0.25f) throttle = 0.0f;
        // Far across? Brake down to walking pace to rotate instead of plowing.
        float aang = ang < 0.0f ? -ang : ang;
        if (aang > 0.9f && speed > 5.0f) {
            brake = 0.8f;
            throttle = 0.0f;
        }
    }

    in->throttle = throttle;
    in->brake = brake;
    in->steer = steer;
    in->handbrake = false;

    // Finish detection
    if (!ai->finished) {
        ai->finish_time += SIM_DT;
        if (ai->line_idx >= (float)t->num_points - 2.0f)
            ai->finished = 1;
    }
}
