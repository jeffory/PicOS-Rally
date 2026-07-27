#!/usr/bin/env python3
"""fetch_assets.py — download PixelLab PNGs into the assets worktree and
build tile sheets + keys.json for tilebake.py.

Usage:
  fetch_assets.py object <url> <out.png>            # single sprite (car, prop)
  fetch_assets.py sheet <out.png> <keys.json> <url1> <url2> ...
      # montage N same-size tiles row-major into one sheet; keys.json must
      # already list the keys in the same order (author it from the
      # PixelLab placement_rules / wang corner metadata).

All downloads go through urllib (stdlib). Sheets are montaged with PIL.
"""
import json
import os
import sys
import urllib.request

from PIL import Image


def fetch(url, out):
    os.makedirs(os.path.dirname(out), exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": "rally-assets/1.0"})
    with urllib.request.urlopen(req, timeout=60) as r, open(out, "wb") as f:
        f.write(r.read())
    print(f"  {os.path.basename(out)} ({os.path.getsize(out)}B)")


def main():
    mode = sys.argv[1]
    if mode == "object":
        url, out = sys.argv[2], sys.argv[3]
        fetch(url, out)
        return
    if mode == "sheet":
        out, keys_path = sys.argv[2], (sys.argv[3] if not sys.argv[3].startswith("http") else None)
        urls = sys.argv[4:] if keys_path else sys.argv[3:]
        if keys_path:
            with open(keys_path) as f:
                keys = json.load(f)
            assert len(keys) == len(urls), f"{len(keys)} keys vs {len(urls)} urls"
        tmp = []
        for i, u in enumerate(urls):
            p = f"/tmp/rally_fetch_{i}.png"
            fetch(u, p)
            tmp.append(Image.open(p).convert("RGBA"))
        w, h = tmp[0].size
        cols = 1
        while cols * cols < len(tmp):
            cols += 1
        sheet = Image.new("RGBA", (cols * w, ((len(tmp) + cols - 1) // cols) * h))
        for i, im in enumerate(tmp):
            sheet.paste(im, ((i % cols) * w, (i // cols) * h))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        sheet.save(out)
        print(f"sheet {out}: {sheet.size[0]}x{sheet.size[1]}, {len(tmp)} tiles, {cols} cols")
        return
    raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
