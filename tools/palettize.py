#!/usr/bin/env python3
"""palettize.py — snap a PixelLab PNG to the locked 48-colour palette.

Usage: palettize.py <in.png> <out.bin> [--name NAME]
Emits a raw 8bpp index stream (row-major, 255=transparent) + a .json sidecar
{"w":..,"h":..} and a manifest.json entry keyed by name (default: out basename).
"""
import json
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
    name = opts.get("name")
    if len(args) != 2:
        raise SystemExit(__doc__)
    src, dst = args
    name = name or os.path.splitext(os.path.basename(dst))[0]

    _, _, colors = rp.load_palette(rp.find_style_toml())
    p = rp.Palettizer(colors)
    img = Image.open(src)
    data, w, h = p.palettize_image(img)
    with open(dst, "wb") as f:
        f.write(data)
    with open(dst + ".json", "w") as f:
        json.dump({"w": w, "h": h}, f)
    rp.update_manifest(
        os.path.join(os.path.dirname(dst), "manifest.json"), name,
        {"src": os.path.basename(src), "src_sha256": rp.sha256_file(src),
         "bin": os.path.basename(dst), "bin_sha256": rp.sha256_file(dst),
         "w": w, "h": h, "fmt": "idx8"})
    print(f"{name}: {w}x{h} -> {dst} ({len(data)}B)")


if __name__ == "__main__":
    main()
