// scene — render the M4 viewport at a given racing-line distance, host-side.
// Usage: scene <dist_m> <out.ppm> [heading_offset_rad]
// Eyeball tool: composites ground tiles + props + car sprite at 320x240 and
// writes a binary PPM (convert with PIL for PNG).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../core/mathx.h"
#include "../../core/sim.h"
#include "../../core/camera.h"
#include "../../core/track.h"
#include "../../core/gfx.h"
#include "../../core/render_track.h"
#include "../../core/tiles_sections.h"

static void *slurp(const char *path, long *sz) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = 0; }
    fclose(f);
    if (sz) *sz = n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: scene <dist_m> <out.ppm> [yaw_offset]\n");
        return 1;
    }
    float dist = (float)atof(argv[1]);
    float yaw_off = argc > 3 ? (float)atof(argv[3]) : 0.0f;
    mx_init();

    long blob_sz;
    uint8_t *blob = slurp("../../stage01.bin", &blob_sz);
    if (!blob) { fprintf(stderr, "no stage01.bin\n"); return 1; }
    track_t track;
    if (!track_load(&track, blob, (int)blob_sz)) { fprintf(stderr, "bad blob\n"); return 1; }

    uint16_t *clut = slurp("../../assets/clut.bin", 0);
    uint8_t *secs[TILE_SEC_COUNT];
    char path[256];
    for (int i = 0; i < TILE_SEC_COUNT; i++) {
        snprintf(path, sizeof(path), "../../assets/tiles_%s.bin", TILE_SEC_NAMES[i]);
        secs[i] = slurp(path, 0);
        if (!secs[i]) { fprintf(stderr, "missing %s\n", path); return 1; }
    }
    uint8_t *car_bin = slurp("../../assets/car.bin", 0);
    uint8_t *props_bin = slurp("../../assets/props.bin", 0);
    if (!clut || !car_bin || !props_bin) { fprintf(stderr, "missing sprites\n"); return 1; }

    gfx_t g;
    gfx_init(&g, clut, (const uint8_t **)secs,
             TILE_SEC_BASES, TILE_SEC_SIZES, TILE_SEC_COUNT);

    float x, y, dx, dy, tv;
    track_line_at(&track, dist / 2.0f, &x, &y, &dx, &dy, &tv);
    car_t car;
    sim_init(&car, x, y, mx_atan2(dx, dy) + yaw_off);
    camera_t cam;
    camera_init(&cam, &car);

    uint16_t *fb = calloc(320 * 240, 2);
    render_track_ground_gfx(&g, fb, &cam, &track, 240);
    render_track_props_gfx(&g, fb, &cam, &track, 240, props_bin, 32);
    // car sprite, same convention as app/main.c
    float h = car.heading;
    while (h < 0.0f) h += 6.2831853f;
    while (h >= 6.2831853f) h -= 6.2831853f;
    int idx = (int)(h * (32.0f / 6.2831853f) + 0.5f) & 31;
    gfx_blit_sprite(&g, fb, 240, car_bin + idx * 32 * 32, 32, 32,
                    160 - 16, 120 - 16);

    FILE *o = fopen(argv[2], "wb");
    fprintf(o, "P6\n320 240\n255\n");
    for (int i = 0; i < 320 * 240; i++) {
        uint16_t v = fb[i];
        // stored big-endian panel order; swap back then expand
        v = (uint16_t)((v << 8) | (v >> 8));
        unsigned char px[3] = {
            (unsigned char)(((v >> 11) & 31) * 255 / 31),
            (unsigned char)(((v >> 5) & 63) * 255 / 63),
            (unsigned char)((v & 31) * 255 / 31) };
        fwrite(px, 1, 3, o);
    }
    fclose(o);
    printf("wrote %s (dist %.0fm, yaw_off %.2f)\n", argv[2], dist, yaw_off);
    return 0;
}
