#!/usr/bin/env python3
"""pixellab_rotate.py — re-view an existing sprite through PixelLab's /rotate.

Why this exists: the M4 car sheet was generated with create_8_direction_object
at a side/low camera, so each of the 8 source views sits at a different
apparent camera elevation (measured: the car's rendered length swings 49→61→52
px across headings). A rigid body under the game's fixed overhead camera must
keep constant dimensions, so the sheet visibly stretches and flips between
roof-on and flank-on as the car turns. Rebaking from a single "high top-down"
view fixes it by construction.

Usage:
  pixellab_rotate.py <in.png> <out.png> [--to-view "high top-down"]
      [--from-view "low top-down"] [--from-dir south] [--to-dir south]
      [--size 96] [--seed N] [--guidance 7.5]

Needs PIXELLAB_API_KEY in the environment. Each call spends one generation.
"""
import base64
import io
import json
import os
import sys
import urllib.error
import urllib.request

API = "https://api.pixellab.ai/v1/rotate"


def b64_png(path):
    with open(path, "rb") as f:
        return base64.b64encode(f.read()).decode()


def rotate(src, dst, to_view, from_view, from_dir, to_dir, size, seed,
           guidance):
    key = os.environ.get("PIXELLAB_API_KEY")
    if not key:
        raise SystemExit("PIXELLAB_API_KEY not set")
    body = {
        "image_size": {"width": size, "height": size},
        "from_image": {"type": "base64", "base64": b64_png(src)},
        "image_guidance_scale": guidance,
    }
    if from_view:
        body["from_view"] = from_view
    if to_view:
        body["to_view"] = to_view
    if from_dir:
        body["from_direction"] = from_dir
    if to_dir:
        body["to_direction"] = to_dir
    if seed is not None:
        body["seed"] = seed
    req = urllib.request.Request(
        API, data=json.dumps(body).encode(),
        headers={"Authorization": f"Bearer {key}",
                 "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=300) as r:
            doc = json.load(r)
    except urllib.error.HTTPError as e:
        raise SystemExit(f"pixellab {e.code}: {e.read().decode()[:600]}")
    img = doc.get("image") or doc.get("images", [{}])[0]
    raw = base64.b64decode(img["base64"])
    with open(dst, "wb") as f:
        f.write(raw)
    usage = doc.get("usage")
    print(f"{dst}: {len(raw)}B  usage={usage}")
    return doc


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
    if len(args) != 2:
        raise SystemExit(__doc__)
    seed = opts.get("seed")
    rotate(args[0], args[1],
           opts.get("to-view", "high top-down"),
           opts.get("from-view"),
           opts.get("from-dir"), opts.get("to-dir"),
           int(opts.get("size", 96)),
           int(seed) if seed is not None else None,
           float(opts.get("guidance", 7.5)))


if __name__ == "__main__":
    main()
