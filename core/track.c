#include "track.h"
#include "sim.h"       // SURF_* ids shared with sim/surface
#include "surface.h"
#include "mathx.h"
#include <string.h>

typedef struct {
    uint32_t magic;
    uint16_t version, num_nodes, num_points, num_notes, num_cps, gw, gh, num_props;
    float ox, oy, cell;
} track_header_t;

bool track_load(track_t *t, const void *blob, int size) {
    if (!blob || size < (int)sizeof(track_header_t)) return false;
    const track_header_t *h = (const track_header_t *)blob;
    if (h->magic != TRACK_MAGIC || (h->version != 1 && h->version != 2)) return false;

    const uint8_t *p = (const uint8_t *)blob + sizeof(track_header_t);
    const uint8_t *end = (const uint8_t *)blob + size;

    t->num_nodes = h->num_nodes;
    t->nodes = (const track_node_t *)p;
    p += h->num_nodes * sizeof(track_node_t);

    t->num_points = h->num_points;
    t->points = (const track_point_t *)p;
    p += h->num_points * sizeof(track_point_t);

    t->num_notes = h->num_notes;
    t->notes = (const track_note_t *)p;
    p += h->num_notes * sizeof(track_note_t);

    t->num_cps = h->num_cps;
    t->cps = (const float *)p;
    p += h->num_cps * sizeof(float);

    if (p > end) return false;
    int grid_bytes = h->gw * h->gh;
    if (p + grid_bytes > end) return false;
    t->grid = p;
    p += grid_bytes;

    t->tilemap = 0;
    t->num_props = 0;
    t->props = 0;
    if (h->version >= 2) {
        int tm_bytes = grid_bytes * TRACK_TILEMAP_SLOTS * 2;
        if (p + tm_bytes > end) return false;
        t->tilemap = (const uint16_t *)p;
        p += tm_bytes;
        if (h->num_props) {
            if (p + h->num_props * sizeof(track_prop_t) > end) return false;
            t->num_props = h->num_props;
            t->props = (const track_prop_t *)p;
        }
    }

    t->ox = h->ox; t->oy = h->oy; t->cell = h->cell;
    t->gw = h->gw; t->gh = h->gh;
    t->length = t->num_points > 0 ? (t->num_points - 1) * 2.0f : 0.0f;
    t->cursor = 0;
    return true;
}

int track_surface_at(const track_t *t, float x, float y) {
    int cx = (int)((x - t->ox) / t->cell);
    int cy = (int)((y - t->oy) / t->cell);
    if (cx < 0 || cy < 0 || cx >= t->gw || cy >= t->gh) return SURF_GRASS;
    return t->grid[cy * t->gw + cx];
}

static int track_src_at_fn(void *ctx, float x, float y) {
    return track_surface_at((const track_t *)ctx, x, y);
}

struct surface_src track_surface_src(track_t *t) {
    struct surface_src s = { track_src_at_fn, t };
    return s;
}

float track_closest(track_t *t, float x, float y, float *out_dist, float *out_halfw) {
    // Local search around the cursor, expanding until a bracket is found.
    int n = t->num_points;
    int c = t->cursor;
    if (c < 0) c = 0;
    if (c > n - 1) c = n - 1;
    float best = 1e30f;
    int best_i = c;
    // widen progressively: ±8, ±32, then full scan as a fallback
    for (int radius = 8; radius <= n; radius *= 4) {
        int lo = c - radius, hi = c + radius;
        if (lo < 0) lo = 0;
        if (hi > n - 1) hi = n - 1;
        for (int i = lo; i <= hi; i++) {
            float dx = t->points[i].x - x;
            float dy = t->points[i].y - y;
            float d2 = dx * dx + dy * dy;
            if (d2 < best) { best = d2; best_i = i; }
        }
        // found an interior minimum → good enough
        if (best_i > lo && best_i < hi) break;
        if (radius >= n) break;
    }
    // Refine to a fractional index: project onto both segments adjacent to
    // the best discrete point and keep the nearer perpendicular foot.
    float frac_idx = (float)best_i;
    float best_d2;
    {
        float dx = t->points[best_i].x - x;
        float dy = t->points[best_i].y - y;
        best_d2 = dx * dx + dy * dy;
    }
    float foot_x = t->points[best_i].x, foot_y = t->points[best_i].y;
    for (int seg = best_i - 1; seg <= best_i; seg++) {
        if (seg < 0 || seg + 1 >= n) continue;
        float sx = t->points[seg + 1].x - t->points[seg].x;
        float sy = t->points[seg + 1].y - t->points[seg].y;
        float len2 = sx * sx + sy * sy;
        if (len2 < 1e-6f) continue;
        float f = ((x - t->points[seg].x) * sx + (y - t->points[seg].y) * sy) / len2;
        f = mx_clamp(f, 0.0f, 1.0f);
        float px = t->points[seg].x + sx * f;
        float py = t->points[seg].y + sy * f;
        float dx = x - px, dy = y - py;
        float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            frac_idx = (float)seg + f;
            foot_x = px; foot_y = py;
        }
    }
    t->cursor = best_i;
    if (out_dist) {
        float g = best_d2;
        // sqrt: Newton from a good seed (best_d2 ≤ ~10^4 in practice)
        float seed = best_d2 > 1.0f ? best_d2 : 1.0f;
        for (int i = 0; i < 3; i++) seed = 0.5f * (seed + g / seed);
        *out_dist = seed;
    }
    (void)foot_x; (void)foot_y;
    if (out_halfw) *out_halfw = t->points[best_i].half_w;
    return frac_idx;
}

void track_line_at(const track_t *t, float idx, float *x, float *y,
                   float *dir_x, float *dir_y, float *target_v) {
    int n = t->num_points;
    int i0 = (int)idx;
    if (i0 < 0) i0 = 0;
    if (i0 > n - 2) i0 = n - 2;
    float f = idx - (float)i0;
    const track_point_t *a = &t->points[i0];
    const track_point_t *b = &t->points[i0 + 1];
    if (x) *x = a->x + (b->x - a->x) * f;
    if (y) *y = a->y + (b->y - a->y) * f;
    float dx = b->x - a->x, dy = b->y - a->y;
    float len = 1e-6f;
    {   // cheap sqrt for 2m steps: ~exact via 2 Newton steps
        float m = dx < 0 ? -dx : dx;
        float n2 = dy < 0 ? -dy : dy;
        if (n2 > m) { float tmp = m; m = n2; n2 = tmp; }
        len = m + n2 * 0.5f;
    }
    if (dir_x) *dir_x = dx / len;
    if (dir_y) *dir_y = dy / len;
    if (target_v) *target_v = a->target_v + (b->target_v - a->target_v) * f;
}

const track_note_t *track_note_at(const track_t *t, float dist) {
    // linear walk is fine (22 notes); binary search if this grows
    for (int i = 0; i < t->num_notes; i++) {
        if (t->notes[i].dist >= dist) return &t->notes[i];
    }
    return 0;
}
