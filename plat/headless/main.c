// Headless harness: runs core/ with no display, no PicOS, no SDL.
// Used by CI: unit tests, determinism replay, completability (M3+).
// M1 scope: scripted drive, state hash print, render smoke (CPU framebuffer).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../core/mathx.h"
#include "../../core/tuning.h"
#include "../../core/sim.h"
#include "../../core/camera.h"
#include "../../core/render.h"

int main(int argc, char **argv) {
    int frames = (argc > 1) ? atoi(argv[1]) : 600;
    mx_init();

    tuning_t tun;
    tuning_defaults(&tun);

    car_t car;
    sim_init(&car, 0.0f, 0.0f, 0.0f);
    camera_t cam;
    camera_init(&cam, &car);

    // CPU framebuffer for render smoke testing.
    uint16_t *fbmem = malloc(320 * 320 * 2);
    if (!fbmem) { fprintf(stderr, "fb alloc failed\n"); return 1; }
    framebuf_t fb = { fbmem, 320, 320 };

    // Procedural texture like the app's.
    uint16_t *tex = malloc(256 * 256 * 2);
    if (!tex) { fprintf(stderr, "tex alloc failed\n"); return 1; }
    for (int i = 0; i < 256 * 256; i++) tex[i] = (uint16_t)(i * 2654435761u >> 16);

    int steps = frames * 2;   // 60 Hz sim, 30 fps frames
    for (int s = 0; s < steps; s++) {
        sim_input_t in = {
            .throttle = 0.75f,
            .brake = 0.0f,
            .steer = mx_sin((float)s * SIM_DT * 0.9f) * 0.85f,
            .handbrake = false,
        };
        sim_step(&car, &in, &tun, SURF_GRAVEL);
        camera_update(&cam, &car, SIM_DT);
        if ((s & 1) == 0) {
            render_ortho_ground(&fb, &cam, tex, 256, 256);
            render_car(&fb, &cam, &car, rgb565_be(28, 4, 4), rgb565_be(31, 63, 31));
        }
    }

    printf("HEADLESS frames=%d steps=%d\n", frames, steps);
    printf("  car: x=%.2f y=%.2f heading=%.3f vx=%.2f vy=%.2f\n",
           car.x, car.y, car.heading, car.vx, car.vy);
    printf("  hash: %08lx\n", (unsigned long)sim_state_hash(&car));
    printf("  fb[centre]=%04x\n", fbmem[160 * 320 + 160]);

    free(tex);
    free(fbmem);
    return 0;
}
