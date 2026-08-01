#!/usr/bin/env python3
"""procgen_road.py — procedural road tiles (fill + smooth edges), palette-locked.

PixelLab's RPG path tiles carry baked decorative borders and assume a
1-tile-wide path — unusable across a 2.25-cell rally corridor. The road is
therefore procedural: a speckled fill per road surface, plus a set of edge
tiles the renderer masked-blits where the corridor boundary crosses a cell.

Edges (M6 polishing run). The first cut gave every boundary cell one 50/50
checker tile, so the road's outline could only ever land on a cell boundary —
a diagonal came out as 16 px stair-steps, which reads blocky and vector-like.

Marching squares over the cell corners was tried next and is not enough here:
the corridor is only ~2.25 cells wide, so on a diagonal a cell often has just
one corner inside and the road breaks into disconnected blobs (measured: 419
single-corner cells on stage01). Corner membership under-samples a band that
thin.

So an edge cell instead stores the boundary itself: the road edge is locally a
straight line, and each tile is that half-plane rasterised at a quantised
(angle, offset). trackbake derives both analytically from the nearest racing
line point, so the cut lands on the true boundary at any road width or angle.
An ordered Bayer band along the cut scatters a few pixels of gravel either
side, so the shoulder reads loose rather than vector-sharp.

Per surface: 1 `roadfill_<surf>` plus ANGLES*OFFSETS `roadcut_<surf>_<a>_<t>`.

Usage: procgen_road.py <out.bin>   (writes atlas.json section "proc")
"""
import json
import math
import os
import sys

import rallypalette as rp

TILE = 16

# Ordered dither for the edge band. A plain hard threshold would give a clean
# but synthetic vector cut; scattering a few pixels either side of it reads as
# loose gravel at the road's shoulder.
BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]

# Edge cut quantisation. Angle error is +/-(180/ANGLES) degrees, which across
# half a tile displaces the cut by ~1.6 px at 16 angles; offsets step 2 px over
# the tile's half-diagonal (11.3 px), so a cut is placed to about a pixel.
ANGLES = 16
OFFSETS = 13            # t index 0..12 maps to -12..+12 px
OFF_STEP = 2.0
OFF_MAX = (OFFSETS - 1) / 2 * OFF_STEP      # 12 px
HALF_DIAG = TILE * 0.7072                   # 11.3 px: tile corner from centre
EDGE_BAND = 1.6         # dithered scatter either side of the cut, in pixels


def cut_offset(t_index):
    return (t_index - (OFFSETS - 1) / 2) * OFF_STEP


def is_road_cut(angle_index, t_index, x, y):
    """Is this pixel inside the half-plane for a quantised (angle, offset)?

    The cut's outward normal is `angle_index` of ANGLES around the circle and
    sits `cut_offset` pixels from the tile centre along it; road is the side
    the normal points away from.
    """
    th = 2.0 * math.pi * angle_index / ANGLES
    mx, my = math.cos(th), math.sin(th)
    px = x + 0.5 - TILE / 2.0
    py = y + 0.5 - TILE / 2.0
    # signed distance past the cut: negative is road
    s = (px * mx + py * my) - cut_offset(t_index)
    d = (BAYER4[y & 3][x & 3] + 0.5) / 16.0 - 0.5
    return s < d * EDGE_BAND


def fnv(*vals):
    h = 2166136261
    for v in vals:
        for shift in (0, 8, 16, 24):
            h ^= (v >> shift) & 0xFF
            h = (h * 16777619) & 0xFFFFFFFF
    return h


def main():
    dst = sys.argv[1]
    seed, names, colors = rp.load_palette(rp.find_style_toml())
    idx = {n: i for i, n in enumerate(names)}

    FILLS = {
        "roadfill_gravel":  ("gravel_mid",  "gravel_light", "gravel_deep"),
        "roadfill_bitumen": ("bitumen_mid", "bitumen_light", "bitumen_deep"),
        "roadfill_sand":    ("sand_wet",    "sand_dry",     "sand_shadow"),
    }
    out = bytearray()
    entries = []

    def speckle(x, y, b, l, dk):
        """Road surface grain — identical pattern in every tile so the fill
        stays seamless across cell boundaries."""
        r = fnv(seed + x, y) % 100
        return l if r < 12 else (dk if r < 24 else b)

    for key, (base, light, dark) in FILLS.items():
        surf = key.split("_", 1)[1]
        b, l, dk = idx[base], idx[light], idx[dark]
        for i in range(TILE * TILE):
            out.append(speckle(i % TILE, i // TILE, b, l, dk))
        entries.append({"key": key, "index": len(entries)})
        for a in range(ANGLES):
            for t in range(OFFSETS):
                for i in range(TILE * TILE):
                    x, y = i % TILE, i // TILE
                    out.append(speckle(x, y, b, l, dk)
                               if is_road_cut(a, t, x, y) else rp.TRANSPARENT)
                entries.append({"key": f"roadcut_{surf}_{a}_{t}",
                                "index": len(entries)})

    with open(dst, "wb") as f:
        f.write(bytes(out))

    atlas_path = os.path.join(os.path.dirname(dst), "atlas.json")
    atlas = {}
    if os.path.exists(atlas_path):
        with open(atlas_path) as f:
            atlas = json.load(f)
    atlas["proc"] = {"bin": os.path.basename(dst), "tile": TILE,
                     "entries": entries}
    with open(atlas_path, "w") as f:
        json.dump(atlas, f, indent=1, sort_keys=True)
        f.write("\n")

    rp.update_manifest(
        os.path.join(os.path.dirname(dst), "manifest.json"), "proc",
        {"src": "procgen_road.py", "bin": os.path.basename(dst),
         "bin_sha256": rp.sha256_file(dst),
         "tiles": len(entries), "tile_px": TILE, "fmt": "idx8-tiles"})
    print(f"proc: {len(entries)} tiles -> {dst} ({len(out)}B)")


if __name__ == "__main__":
    main()
