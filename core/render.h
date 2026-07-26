// render — orthographic ground blitter + grey-box car, on a raw RGB565
// big-endian framebuffer (the PicOS back buffer format). Pure C, no PicOS
// types: the app layer hands us a pointer and the camera/sim state.
//
// Projection lives behind one interface (§4a): the app picks per frame —
// render_ortho_ground (this file) or the firmware drawPlane (app-side).
#pragma once

#include <stdint.h>
#include "sim.h"
#include "camera.h"

// World scale: 4 px per metre (§6). 16 px tile = 4 m.
#define RENDER_PX_PER_M 4.0f

typedef struct {
    uint16_t *fb;       // big-endian RGB565
    int w, h;           // 320x320
} framebuf_t;

static inline uint16_t rgb565_be(int r, int g, int b) {
    uint16_t v = (uint16_t)(((r) & 0x1F) << 11 | ((g) & 0x3F) << 5 | ((b) & 0x1F));
    return (uint16_t)((v >> 8) | (v << 8));
}

void render_clear(framebuf_t *f, uint16_t be_color);
// Blit the ground: tex is a tw×th wrapping texture (host-order RGB565),
// sampled 1:1 at RENDER_PX_PER_M. Column-major-ish row blits with wrap.
void render_ortho_ground(framebuf_t *f, const camera_t *cam,
                         const uint16_t *tex, int tw, int th);
// Grey-box car: filled rect + rotated outline + heading nose.
void render_car(framebuf_t *f, const camera_t *cam, const car_t *car,
                uint16_t be_body, uint16_t be_outline);
// Bresenham line into the framebuffer (clipped).
void render_line(framebuf_t *f, int x0, int y0, int x1, int y1, uint16_t be_color);
// 6×8 text (font6x8, firmware-compatible layout). be_bg < 0 = transparent.
void render_text(framebuf_t *f, int x, int y, const char *text,
                 uint16_t be_fg, int32_t be_bg);
void render_fill_rect(framebuf_t *f, int x, int y, int w, int h, uint16_t be_color);
