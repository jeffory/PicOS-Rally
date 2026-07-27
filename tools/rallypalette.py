#!/usr/bin/env python3
"""Shared palette/CLUT library for the PicOS Rally asset pipeline.

Loads assets/style.toml (48 locked colors), derives the 256-entry CLUT per
the layout documented in style.toml, and snaps RGB(A) pixels to the locked
palette (no dithering). Used by palettize.py, spritebake.py, tilebake.py.
"""
import hashlib
import json
import os
import struct
import tomllib

TRANSPARENT = 255  # sprite mask key


def update_manifest(manifest_path, name, entry):
    doc = {}
    if os.path.exists(manifest_path):
        with open(manifest_path) as f:
            doc = json.load(f)
    doc[name] = entry
    with open(manifest_path, "w") as f:
        json.dump(doc, f, indent=2, sort_keys=True)
        f.write("\n")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def load_palette(style_path):
    with open(style_path, "rb") as f:
        doc = tomllib.load(f)
    seed = doc["palette"]["seed"]
    colors = []
    names = []
    for name, hexv in doc["palette"].items():
        if name == "seed":
            continue
        names.append(name)
        colors.append(tuple(int(hexv[i:i + 2], 16) for i in (1, 3, 5)))
    assert len(colors) == 48, f"expected 48 colors, got {len(colors)}"
    return seed, names, colors


def derive_clut(colors):
    """48 locked + 4 shade steps each (192) + reserved. Returns 256 (r,g,b)."""
    clut = list(colors)
    for (r, g, b) in colors:
        for pct in (-20, -10, 10, 20):
            f = 1.0 + pct / 100.0
            clut.append((min(255, int(r * f)), min(255, int(g * f)),
                         min(255, int(b * f))))
    while len(clut) < 256:
        clut.append((0, 0, 0))
    return clut


def rgb565_be(rgb):
    r, g, b = rgb
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    # the display back buffer holds big-endian values (panel ships bytes
    # as-is): store the byte-swapped form so blits can copy verbatim.
    return ((v & 0xFF) << 8) | (v >> 8)


def write_clut_bin(colors, path):
    """256-entry CLUT as byte-swapped RGB565 u16s (back-buffer order)."""
    clut = derive_clut(colors)
    with open(path, "wb") as f:
        for rgb in clut:
            f.write(struct.pack("<H", rgb565_be(rgb)))


class Palettizer:
    """Snap pixels to the 48 locked colors. Cache-backed nearest match."""

    def __init__(self, colors):
        self.colors = colors
        self._cache = {}

    def index(self, rgb):
        c = self._cache.get(rgb)
        if c is not None:
            return c
        r, g, b = rgb
        best, bestd = 0, 1 << 62
        for i, (cr, cg, cb) in enumerate(self.colors):
            # perceptual-ish weighting: green matters most
            d = 3 * (r - cr) * (r - cr) + 6 * (g - cg) * (g - cg) + 2 * (b - cb) * (b - cb)
            if d < bestd:
                best, bestd = i, d
        self._cache[rgb] = best
        return best

    def palettize_image(self, img):
        """PIL RGBA image -> 8bpp PIL image ('P' mode raw indices) honoring alpha."""
        img = img.convert("RGBA")
        w, h = img.size
        out = bytearray(w * h)
        src = img.tobytes()
        for i in range(w * h):
            a = src[i * 4 + 3]
            if a < 128:
                out[i] = TRANSPARENT
            else:
                out[i] = self.index((src[i * 4], src[i * 4 + 1], src[i * 4 + 2]))
        return bytes(out), w, h


def find_style_toml():
    here = os.path.dirname(os.path.abspath(__file__))
    # tools/ is at apps/rally/tools; assets worktree is a sibling checkout.
    candidates = [
        os.path.join(here, "..", "..", "..", "..", "picos-rally-assets", "assets", "rally", "style.toml"),
        os.path.join(here, "..", "..", "..", "picos-rally-assets", "assets", "rally", "style.toml"),
        os.environ.get("RALLY_STYLE", ""),
    ]
    for c in candidates:
        if c and os.path.exists(c):
            return os.path.abspath(c)
    raise SystemExit("style.toml not found; set RALLY_STYLE")


def png_dir():
    return os.path.join(os.path.dirname(find_style_toml()), "png")
