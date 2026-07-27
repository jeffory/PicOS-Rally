#!/usr/bin/env python3
"""spritebake.py — bake a single-heading top-down sprite into a 32-heading
rotation sheet for the rally car (or any rotating entity).

Pipeline per heading: rotate source at full res (PIL bicubic) -> downsample to
the target size (rotate-then-downsample keeps edges clean) -> palettize.

Usage: spritebake.py <in.png> <out.bin> [--size 48] [--headings 32] [--name NAME]
       spritebake.py --sources src.json <out.bin> [--size 32] [--headings 32]
out.bin = headings * size * size bytes of 8bpp indices, heading 0 = source
orientation, stepping with yaw convention (k * 360/headings degrees, PIL CCW
rotation matches sim yaw = atan2(dx,dy)).

src.json (multi-source mode): [{"file": "car_south.png", "deg": 0}, ...] —
each heading picks the nearest source by circular distance and rotates only
the delta (keeps PixelLab 8-direction art crisp at 32 headings).
"""
import json
import math
import os
import sys

from PIL import Image

import rallypalette as rp


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
    size = int(opts.get("size", 48))
    headings = int(opts.get("headings", 32))
    name = opts.get("name")
    sources_path = opts.get("sources")
    if sources_path:
        if len(args) != 1:
            raise SystemExit(__doc__)
        dst = args[0]
        name = name or os.path.splitext(os.path.basename(dst))[0]
        with open(sources_path) as f:
            sources = json.load(f)
        _, _, colors = rp.load_palette(rp.find_style_toml())
        p = rp.Palettizer(colors)
        loaded = []
        for s in sources:
            sp = s["file"]
            if not os.path.isabs(sp):
                sp = os.path.join(os.path.dirname(sources_path), sp)
            loaded.append((float(s["deg"]), Image.open(sp).convert("RGBA")))
        out = bytearray()
        for k in range(headings):
            yaw = 360.0 * k / headings
            deg, img = min(loaded, key=lambda sd: abs((yaw - sd[0] + 180) % 360 - 180))
            delta = (yaw - deg + 180) % 360 - 180
            side = min(img.size)
            sq = img.crop(((img.size[0] - side) // 2, (img.size[1] - side) // 2,
                           (img.size[0] + side) // 2, (img.size[1] + side) // 2))
            rot = sq.rotate(delta, resample=Image.BICUBIC, expand=False)
            small = rot.resize((size, size), Image.LANCZOS)
            data, _, _ = p.palettize_image(small)
            out += data
        _write_out(dst, name, out, size, headings,
                   os.path.basename(sources_path))
        return
    if len(args) != 2:
        raise SystemExit(__doc__)
    src, dst = args
    name = name or os.path.splitext(os.path.basename(dst))[0]

    _, _, colors = rp.load_palette(rp.find_style_toml())
    p = rp.Palettizer(colors)
    img = Image.open(src).convert("RGBA")

    # centre-crop square at native res so rotation doesn't clip corners
    w, h = img.size
    side = min(w, h)
    img = img.crop(((w - side) // 2, (h - side) // 2,
                    (w + side) // 2, (h + side) // 2))

    out = bytearray()
    for k in range(headings):
        deg = 360.0 * k / headings  # yaw convention (PIL CCW matches sim yaw)
        rot = img.rotate(deg, resample=Image.BICUBIC, expand=False)
        small = rot.resize((size, size), Image.LANCZOS)
        data, _, _ = p.palettize_image(small)
        out += data

    _write_out(dst, name, out, size, headings, os.path.basename(src))


def _write_out(dst, name, out, size, headings, src_label):
    src = os.path.join(os.path.dirname(dst), os.path.basename(src_label))
    with open(dst, "wb") as f:
        f.write(bytes(out))
    with open(dst + ".json", "w") as f:
        json.dump({"size": size, "headings": headings}, f)
    entry = {"src": os.path.basename(src_label),
             "bin": os.path.basename(dst), "bin_sha256": rp.sha256_file(dst),
             "w": size, "h": size, "frames": headings, "fmt": "idx8-sheet"}
    if os.path.exists(src):
        entry["src_sha256"] = rp.sha256_file(src)
    rp.update_manifest(os.path.join(os.path.dirname(dst), "manifest.json"),
                       name, entry)
    print(f"{name}: {headings} headings x {size}x{size} -> {dst} ({len(out)}B)")


if __name__ == "__main__":
    main()
