#!/usr/bin/env python3
"""pixellab_gen.py — text→sprite via PixelLab pixflux, palette-locked.

Used to source the true top-down car view the M6 polishing run needs: the
M4 8-direction sheet was generated at a side/low camera, so its apparent
elevation swings per heading and the car stretches as it turns (see
pixellab_rotate.py). A top-down plan view can be rotated rigidly through all
32 headings instead.

The locked 48-colour style palette is passed as `color_image` so output lands
close to the palette before rallypalette snaps it.

Usage:
  pixellab_gen.py <out.png> --desc "..." [--view "high top-down"]
      [--dir south] [--size 64] [--seed N] [--guidance 8]
      [--init in.png] [--init-strength 300] [--no-palette]
      [--palette car|name,name,...]

Needs PIXELLAB_API_KEY. Each call spends one generation.
"""
import base64
import io
import json
import os
import sys
import urllib.error
import urllib.request

from PIL import Image

import rallypalette as rp

API = "https://api.pixellab.ai/v1/generate-image-pixflux"


# A car may only be painted in car/neutral colours. Offering the whole 48
# lets the model reach for terrain hues — the first top-down bake came back
# with water_light/water_mid/creek as a cool rim-shadow, which reads as a
# teal halo once the sprite is rotating on gravel.
CAR_PALETTE = [
    "car_white", "car_red", "car_blue", "car_black", "car_glass",
    "car_chrome", "sign_red", "line_white", "hud_text", "crest_flash",
    "bitumen_deep", "bitumen_mid", "bitumen_light", "rock_grey",
    "shadow_dark", "skid_dark", "hud_panel", "hud_panel_lit",
    "hud_amber", "hud_amber_dim",
]
SUBSETS = {"car": CAR_PALETTE}


def palette_image_b64(subset=None):
    """Locked colours as a small PNG — PixelLab's forced palette input."""
    _, names, colors = rp.load_palette(rp.find_style_toml())
    if subset:
        want = SUBSETS.get(subset)
        if want is None:
            want = [s.strip() for s in subset.split(",") if s.strip()]
        missing = [n for n in want if n not in names]
        if missing:
            raise SystemExit(f"unknown palette colours: {missing}")
        colors = [colors[names.index(n)] for n in want]
    side = max(1, int(len(colors) ** 0.5 + 0.999))
    img = Image.new("RGB", (side, side))
    data = [tuple(c) for c in colors]
    data += [data[-1]] * (side * side - len(data))
    img.putdata(data)
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode()


def b64_png(path):
    with open(path, "rb") as f:
        return base64.b64encode(f.read()).decode()


def generate(dst, desc, view, direction, size, seed, guidance, init,
             init_strength, use_palette, subset=None):
    key = os.environ.get("PIXELLAB_API_KEY")
    if not key:
        raise SystemExit("PIXELLAB_API_KEY not set")
    body = {
        "description": desc,
        "image_size": {"width": size, "height": size},
        "text_guidance_scale": guidance,
        "no_background": True,
    }
    if view:
        body["view"] = view
    if direction:
        body["direction"] = direction
    if seed is not None:
        body["seed"] = seed
    if use_palette:
        body["color_image"] = {"type": "base64",
                               "base64": palette_image_b64(subset)}
    if init:
        body["init_image"] = {"type": "base64", "base64": b64_png(init)}
        body["init_image_strength"] = init_strength
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
    print(f"{dst}: {len(raw)}B usage={doc.get('usage')}")


def main():
    args, opts = [], {}
    i = 1
    while i < len(sys.argv):
        a = sys.argv[i]
        if a == "--no-palette":
            opts["no-palette"] = "1"
            i += 1
        elif a.startswith("--"):
            opts[a[2:]] = sys.argv[i + 1]
            i += 2
        else:
            args.append(a)
            i += 1
    if len(args) != 1 or "desc" not in opts:
        raise SystemExit(__doc__)
    seed = opts.get("seed")
    generate(args[0], opts["desc"],
             opts.get("view", "high top-down"), opts.get("dir"),
             int(opts.get("size", 64)),
             int(seed) if seed is not None else None,
             float(opts.get("guidance", 8)),
             opts.get("init"), int(opts.get("init-strength", 300)),
             not opts.get("no-palette"), opts.get("palette"))


if __name__ == "__main__":
    main()
