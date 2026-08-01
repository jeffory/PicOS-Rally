// perf — drive-feel measurements against the real sim, for tuning passes.
// Prints standing-start acceleration, braking distance and a steady-state
// steer response so a handling.toml change can be judged on numbers rather
// than feel alone. Usage: ./perf [tuning.toml]
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../../core/mathx.h"
#include "../../core/sim.h"
#include "../../core/tuning.h"

static void accel(const tuning_t *t, float target, const char *label) {
    car_t c;
    sim_init(&c, 0, 0, 0);
    sim_input_t in = {0};
    in.throttle = 1.0f;
    float peak = 0.0f;
    for (int i = 0; i < 60 * 60; i++) {
        sim_step(&c, &in, t, 0);
        if (c.accel_lon > peak) peak = c.accel_lon;
        if (c.vx >= target) {
            printf("  0-%-3.0f km/h : %5.2f s   (peak a %.2f m/s2)\n",
                   target * 3.6f, i / 60.0f, peak);
            return;
        }
    }
    printf("  0-%-3.0f km/h : never (top %.1f km/h)\n", target * 3.6f,
           c.vx * 3.6f);
}

static void braking(const tuning_t *t, float from) {
    car_t c;
    sim_init(&c, 0, 0, 0);
    sim_input_t in = {0};
    in.throttle = 1.0f;
    for (int i = 0; i < 60 * 60 && c.vx < from; i++) sim_step(&c, &in, t, 0);
    float x0 = c.x, y0 = c.y;
    in.throttle = 0.0f;
    in.brake = 1.0f;
    for (int i = 0; i < 60 * 30 && c.vx > 1.0f; i++) sim_step(&c, &in, t, 0);
    float d = sqrtf((c.x - x0) * (c.x - x0) + (c.y - y0) * (c.y - y0));
    printf("  brake %3.0f km/h -> 3.6 km/h : %5.1f m\n", from * 3.6f, d);
}

static void steering(const tuning_t *t, float speed) {
    car_t c;
    sim_init(&c, 0, 0, 0);
    sim_input_t in = {0};
    in.throttle = 1.0f;
    for (int i = 0; i < 60 * 60 && c.vx < speed; i++) sim_step(&c, &in, t, 0);
    // Hold the target speed through the manoeuvre. Steering in while still
    // accelerating transfers load off the front axle, so an open-throttle
    // test measures engine tune rather than steering tune — and a stronger
    // engine then looks like worse turning.
    float t90 = -1.0f, h0 = 0.0f, peak_yaw = 0.0f, turned = 0.0f;
    for (int i = 0; i < 60 * 5; i++) {
        float err = speed - c.vx;
        in.throttle = mx_clamp(err * 0.5f, 0.0f, 1.0f);
        in.brake = mx_clamp(-err * 0.5f, 0.0f, 1.0f);
        if (i == 30) { in.steer = 1.0f; h0 = c.heading; }
        float prev = c.heading;
        sim_step(&c, &in, t, 0);
        if (i < 30) continue;
        if (c.yaw_rate > peak_yaw) peak_yaw = c.yaw_rate;
        float d = c.heading - prev;
        while (d < -3.14159f) d += 6.28318f;
        while (d > 3.14159f) d -= 6.28318f;
        turned += d;
        if (t90 < 0 && turned >= 1.5708f) t90 = (i - 30) / 60.0f;
    }
    printf("  %3.0f km/h: peak yaw %.2f rad/s, 90 deg in ", speed * 3.6f,
           peak_yaw);
    if (t90 >= 0) printf("%.2f s", t90);
    else printf("never");
    printf("   (held %.0f km/h)\n", c.vx * 3.6f);
}

int main(int argc, char **argv) {
    mx_init();
    tuning_t t;
    tuning_defaults(&t);
    const char *path = argc > 1 ? argv[1] : "../../tuning/handling.toml";
    FILE *f = fopen(path, "rb");
    if (f) {
        static char buf[8192];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
        tuning_parse(&t, buf, (int)n, 0);
        printf("tuning: %s\n", path);
    } else {
        printf("tuning: built-in defaults (%s not found)\n", path);
    }
    printf("mass %.0f kg  engine %.0f N  brake %.0f N  vmax %.1f m/s  mu %.2f\n",
           t.mass, t.engine_force, t.brake_force, t.max_speed, t.mu);
    printf("acceleration (uniform gravel):\n");
    accel(&t, 27.8f, "0-100");
    accel(&t, 38.9f, "0-140");
    printf("braking:\n");
    braking(&t, 27.8f);
    printf("steering:\n");
    steering(&t, 15.0f);
    steering(&t, 30.0f);
    return 0;
}
