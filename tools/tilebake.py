#!/usr/bin/env python3
"""tilebake.py — slice a PixelLab tileset/path sheet into 16px 8bpp tiles.

Usage: tilebake.py <in.png> <out.bin> --keys keys.json [--src-tile 32] [--cols N]
           [--name NAME]
       tilebake.py <in.png> <out.bin> --meta metadata.json [--name NAME]
           # PixelLab Wang mode: keys + crop boxes come from the tileset
           # metadata JSON (corners NW,NE,SW,SE; bounding_box in the sheet).
           # Do NOT slice Wang sheets positionally — see PixelLab docs.

keys.json: ordered list of semantic keys, one per output tile (out.bin local
index = position in this list).
  Wang set:   {"key": "wang:TL,TR,BL,BR"} with 1 = upper terrain, e.g. "wang:0,1,1,0"
  Path set:   {"key": "path:<mask>"} mask bit0=N bit1=E bit2=S bit3=W
  Variant:    {"key": "pathvar:<mask>"} alternate art for the same mask
  Plain:      {"key": "base:grass"}
Each entry may set "src" (index of the tile in the source sheet, default =
its own position) and "rot" (0/90/180/270 CW degrees, default 0) — used to
synthesize masks PixelLab didn't ship by rotating donor tiles. Entries may
also set "crop": [x, y, w, h] (source-tile px) to bake a sub-rectangle —
used for "roadfill" interior tiles (centre crop of a straight road tile).

out.bin = N * 16 * 16 bytes of 8bpp indices (local index order = keys order).
Writes out.json {"tile":16,"count":N} and appends to atlas.json next to out.bin
(trackbake reads every atlas.json section to assign global tile indices).
"""
import json
import os
import sys

from PIL import Image

import rallypalette as rp

TILE = 16


def main():
    args, opts = [], {}
    i = 1
    while i < len(sys.argv):
        if sys.argv[i].startswith("--"):
            opts[sys.argv[i][2:]] = sys.argv[i + 1]
            i += 2
        else:
            args.append(sys.argv[i])
            i += 1
    src_tile = int(opts.get("src-tile", 32))
    cols = int(opts["cols"]) if "cols" in opts else None
    name = opts.get("name")
    keys_path = opts.get("keys")
    meta_path = opts.get("meta")
    if len(args) != 2 or (not keys_path and not meta_path):
        raise SystemExit(__doc__)
    src, dst = args
    name = name or os.path.splitext(os.path.basename(dst))[0]

    boxes = None
    if meta_path:
        with open(meta_path) as f:
            meta = json.load(f)
        tiles = meta["tileset_data"]["tiles"]
        keys = []
        boxes = []
        for t in tiles:
            c = t["corners"]
            key = "wang:%d,%d,%d,%d" % (
                c["NW"] == "upper", c["NE"] == "upper",
                c["SW"] == "upper", c["SE"] == "upper")
            keys.append({"key": key})
            bb = t["bounding_box"]
            boxes.append((bb["x"], bb["y"], bb["width"]))
        n = len(keys)
    else:
        with open(keys_path) as f:
            keys = json.load(f)
        n = len(keys)

    _, _, colors = rp.load_palette(rp.find_style_toml())
    p = rp.Palettizer(colors)
    img = Image.open(src).convert("RGBA")
    w, h = img.size
    cols = cols or (w // src_tile)
    rows = (n + cols - 1) // cols

    out = bytearray()
    for t in range(n):
        k = keys[t]
        if boxes is not None:
            bx, by, bw = boxes[t]
            tile = img.crop((bx, by, bx + bw, by + bw))
            this_src = bw
        else:
            si = k.get("src", t)
            cx, cy = (si % cols) * src_tile, (si // cols) * src_tile
            tile = img.crop((cx, cy, cx + src_tile, cy + src_tile))
            this_src = src_tile
        rot = k.get("rot", 0)
        if rot:
            tile = tile.rotate(-rot, resample=Image.NEAREST, expand=False)
        if "crop" in k:
            cx0, cy0, cw, ch = k["crop"]
            tile = tile.crop((cx0, cy0, cx0 + cw, cy0 + ch))
            this_src = cw
        if this_src != TILE:
            tile = tile.resize((TILE, TILE), Image.LANCZOS)
        data, _, _ = p.palettize_image(tile)
        out += data

    with open(dst, "wb") as f:
        f.write(bytes(out))
    with open(dst + ".json", "w") as f:
        json.dump({"tile": TILE, "count": n}, f)

    atlas_path = os.path.join(os.path.dirname(dst), "atlas.json")
    atlas = {}
    if os.path.exists(atlas_path):
        with open(atlas_path) as f:
            atlas = json.load(f)
    atlas[name] = {"bin": os.path.basename(dst), "tile": TILE,
                   "entries": [{"key": k["key"], "index": i}
                               for i, k in enumerate(keys)]}
    with open(atlas_path, "w") as f:
        json.dump(atlas, f, indent=1, sort_keys=True)
        f.write("\n")

    rp.update_manifest(
        os.path.join(os.path.dirname(dst), "manifest.json"), name,
        {"src": os.path.basename(src), "src_sha256": rp.sha256_file(src),
         "bin": os.path.basename(dst), "bin_sha256": rp.sha256_file(dst),
         "tiles": n, "tile_px": TILE, "fmt": "idx8-tiles"})
    print(f"{name}: {n} tiles {TILE}px -> {dst} ({len(out)}B)")


if __name__ == "__main__":
    main()
