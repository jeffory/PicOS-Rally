// track — baked stage blob: loader + queries. Parses in place (the blob is
// one PSRAM allocation; records are 4-aligned by construction).
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define TRACK_MAGIC 0x47545352u  // 'RSTG'

// blob v2: after the surface grid come gw*gh*TRACK_TILEMAP_SLOTS u16 tilemap
// entries (global tile indices, 0xFFFF = skip) then num_props prop records.
#define TRACK_TILEMAP_SLOTS 5
#define TRACK_NO_TILE 0xFFFF

typedef struct { float x, y, width; uint8_t surface, flags, _pad[2]; } track_node_t;
typedef struct { float x, y, target_v, half_w; uint8_t surface, flags, _pad[2]; } track_point_t;
typedef struct { float dist; uint8_t len; char text[31]; } track_note_t;
typedef struct { int32_t x_dm, y_dm; uint8_t type, _pad[3]; } track_prop_t;

typedef struct {
    // parsed views into the blob
    int num_nodes;   const track_node_t  *nodes;
    int num_points;  const track_point_t *points;   // racing line, ~2 m spacing
    int num_notes;   const track_note_t  *notes;
    int num_cps;     const float         *cps;      // checkpoint dists
    // surface grid
    float ox, oy, cell;
    int gw, gh;
    const uint8_t *grid;
    // v2: baked tilemap + props (NULL/0 on v1 blobs)
    const uint16_t *tilemap;   // gw*gh*TRACK_TILEMAP_SLOTS global tile indices
    int num_props;
    const track_prop_t *props;
    float length;                                   // stage length in m
    // cursor for accelerated closest-point queries (monotonic progress)
    int cursor;
} track_t;

// Parse a blob loaded into memory. Returns true on valid magic/version.
// The blob must outlive the track_t (no copy).
bool track_load(track_t *t, const void *blob, int size);

// Surface (SURF_*) at world position — the per-wheel physics lookup.
int  track_surface_at(const track_t *t, float x, float y);

// surface_src_t provider over the track grid (surface_src_t from surface.h).
struct surface_src;
struct surface_src track_surface_src(track_t *t);

// Closest racing-line point to (x, y). Returns the fractional point index
// (integer part = segment start); *out_dist = lateral distance in m,
// *out_halfw = road half-width at that point. Uses the cursor as a hint —
// calls with monotonic progress are O(1) amortized.
float track_closest(track_t *t, float x, float y, float *out_dist, float *out_halfw);

// Interpolated racing-line state at fractional index (for respawn/AI).
void track_line_at(const track_t *t, float idx, float *x, float *y,
                   float *dir_x, float *dir_y, float *target_v);

// Next pacenote at or after `dist`, or NULL. Notes are distance-sorted.
const track_note_t *track_note_at(const track_t *t, float dist);
