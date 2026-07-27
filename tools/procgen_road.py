#!/usr/bin/env python3
"""procgen_road.py — procedural road tiles (fill + edge dither), palette-locked.

PixelLab's RPG path tiles carry baked decorative borders and assume a
1-tile-wide path — unusable across a 2.25-cell rally corridor. The road is
therefore procedural: a speckled fill per road surface, and 50/50 dither
tiles (road colour in the pattern, transparent elsewhere) that the renderer
masked-blits over verge cells to soften the edge.

Usage: procgen_road.py <out.bin>   (writes atlas.json section "proc")
"""
import json
import os
import sys

import rallypalette as rp

TILE = 16


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
    DITHERS = {
        "dither_gravel":  "gravel_mid",
        "dither_bitumen": "bitumen_mid",
        "dither_sand":    "sand_wet",
    }

    out = bytearray()
    entries = []

    for key, (base, light, dark) in FILLS.items():
        b, l, dk = idx[base], idx[light], idx[dark]
        for i in range(TILE * TILE):
            x, y = i % TILE, i // TILE
            r = fnv(seed + x, y) % 100
            if r < 12:
                out.append(l)
            elif r < 24:
                out.append(dk)
            else:
                out.append(b)
        entries.append({"key": key, "index": len(entries)})

    for key, cname in DITHERS.items():
        c = idx[cname]
        for i in range(TILE * TILE):
            x, y = i % TILE, i // TILE
            # 50/50 checker dither, phase-broken so it doesn't moire
            on = ((x + y) & 1) ^ ((fnv(seed, x, y) >> 3) & 1)
            out.append(c if on else rp.TRANSPARENT)
        entries.append({"key": key, "index": len(entries)})

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
