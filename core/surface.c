// The test oval, centred on the world origin (metres, heading convention
// 0=+Y north, clockwise): gravel/bitumen ring, creek, sand trap, grass.
#include "surface.h"
#include "sim.h"
#include "mathx.h"

#define OVAL_R_IN   50.0f
#define OVAL_R_OUT  60.0f
#define SAND_R_OUT  66.0f
#define CREEK_R_IN  46.0f
#define CREEK_R_OUT 64.0f
#define CREEK_ANGLE 225.0f
#define CREEK_HALF  6.0f   // M2.3: 3° was a 0.2s blip at speed — readable now

void surface_init_test_oval(surface_map_t *m) {
    m->unused = 0;
}

// Cheap deterministic sqrt: power-of-two bracket then Newton refinements.
static float s_sqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    float g = 1.0f;
    while (g * g < x) g *= 2.0f;    // bracket: g/√2 < √x ≤ g
    for (int i = 0; i < 6; i++) g = 0.5f * (g + x / g);
    return g;
}

int surface_at(const surface_map_t *m, float x, float y) {
    (void)m;
    float r = s_sqrt(x * x + y * y);
    // Angle in heading convention: 0 = +Y (north), clockwise → mx_atan2(x, y).
    float deg = mx_atan2(x, y) * (180.0f / MX_PI);
    if (deg < 0.0f) deg += 360.0f;

    // Creek band crosses everything (check first)
    float dcreek = deg - CREEK_ANGLE;
    if (dcreek > 180.0f) dcreek -= 360.0f;
    if (dcreek < -180.0f) dcreek += 360.0f;
    if (r >= CREEK_R_IN && r <= CREEK_R_OUT &&
        dcreek >= -CREEK_HALF && dcreek <= CREEK_HALF)
        return SURF_WATER;

    // Sand trap (outside runoff)
    if (r > OVAL_R_OUT && r <= SAND_R_OUT && deg >= 180.0f && deg < 270.0f)
        return SURF_SAND;

    // The ring itself
    if (r >= OVAL_R_IN && r <= OVAL_R_OUT) {
        if (deg >= 90.0f && deg < 180.0f)
            return SURF_BITUMEN;
        return SURF_GRAVEL;
    }

    return SURF_GRASS;
}

static int oval_at(void *ctx, float x, float y) {
    return surface_at((surface_map_t *)ctx, x, y);
}

surface_src_t surface_src_oval(surface_map_t *m) {
    surface_src_t s = { oval_at, m };
    return s;
}

float surface_rr_scale(int surface) {
    // M2.3: spread exaggerated so surfaces READ through the seat of your
    // pants (bitumen reference = 1.0). Sand/water should visibly bog you.
    switch (surface) {
    case SURF_BITUMEN: return 1.0f;
    case SURF_GRAVEL:  return 1.3f;
    case SURF_SAND:    return 3.5f;
    case SURF_GRASS:   return 2.0f;
    case SURF_MUD:     return 3.0f;
    case SURF_WATER:   return 5.0f;
    default:           return 1.0f;
    }
}

#define RGB565H(r,g,b) ((uint16_t)(((r)&0x1F)<<11 | ((g)&0x3F)<<5 | ((b)&0x1F)))

void surface_paint_texture(const surface_map_t *m, uint16_t *tex,
                           int tex_w, int tex_h, int px_per_m) {
    const int cx = tex_w / 2, cy = tex_h / 2;
    for (int ty = 0; ty < tex_h; ty++) {
        for (int tx = 0; tx < tex_w; tx++) {
            float wx = (float)(tx - cx) / (float)px_per_m;
            float wy = (float)(ty - cy) / (float)px_per_m;
            int s = surface_at(m, wx, wy);
            uint16_t c;
            switch (s) {
            case SURF_BITUMEN: c = RGB565H(7, 8, 9);    break;  // dark grey
            case SURF_GRAVEL:  c = RGB565H(13, 8, 5);   break;  // red ochre
            case SURF_SAND:    c = RGB565H(22, 18, 9);  break;  // tan
            case SURF_GRASS:   c = RGB565H(4, 13, 5);   break;  // scrub green
            case SURF_MUD:     c = RGB565H(8, 6, 4);    break;
            case SURF_WATER:   c = RGB565H(3, 14, 18);  break;  // turquoise
            default:           c = RGB565H(4, 13, 5);   break;
            }
            // Cheap texture: alternate rows dithered one shade darker
            if ((ty & 3) == 3 && s != SURF_WATER)
                c = (uint16_t)((c & 0xF7DE) >> 1);
            tex[ty * tex_w + tx] = c;
        }
    }
}
