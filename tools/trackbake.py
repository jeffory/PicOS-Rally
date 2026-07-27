#!/usr/bin/env python3
"""trackbake — bake a PicOS Rally stage from tracks/<stage>.toml into the
binary blob the game loads from SD.

Everything the spec §8 requires is baked offline:
  - spline nodes (Catmull-Rom centreline, arc-length resampled)
  - racing line + per-point target speed (curvature + mu)
  - surface grid (coarse cells for per-wheel physics lookup)
  - pacenotes (severity 1-6 from radius, tightens/opens from curvature delta)
  - checkpoints (every ~8 s of racing line)
  - off-course width profile (per-line-point half width)
  - tilemap (Wang terrain layers + road overlay per grid cell, blob v2)
  - prop scatter (deterministic, seeded)
  - minimap PPM (host-side eyeball)

Usage:
  trackbake.py profile tracks/<stage>.toml     # print stage profile
  trackbake.py bake   tracks/<stage>.toml out.bin [assets_dir]
  trackbake.py map    tracks/<stage>.toml /tmp/stage.ppm

Deterministic: seeded RNG, sorted iteration, no dict-order dependence.
"""
import json
import math
import os
import struct
import sys

# ── TOML reader (subset: [[nodes]] tables with scalar keys) ─────────────────
def read_toml_nodes(path):
    meta = {"name": "stage"}
    nodes = []
    cur = None
    for raw in open(path, "r", encoding="utf-8"):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line == "[[nodes]]":
            if cur is not None:
                nodes.append(cur)
            cur = {}
            continue
        if "=" not in line:
            continue
        # allow "k = v; k = v; ..." on one line (spec's node style)
        for part in line.split(";"):
            if "=" not in part:
                continue
            key, _, val = (p.strip() for p in part.partition("="))
            val = val.strip('"')
            if cur is None:
                meta[key] = val
            else:
                cur[key] = val
    if cur is not None:
        nodes.append(cur)
    out = []
    for n in nodes:
        out.append({
            "x": float(n["x"]),
            "y": float(n["y"]),
            "width": float(n.get("width", 9.0)),
            "surface": n.get("surface", "gravel"),
            "flags": set(f.strip() for f in n.get("flags", "").split(",") if f.strip()),
        })
    return meta, out


# ── Catmull-Rom centreline ──────────────────────────────────────────────────
def catmull_rom(points, samples_per_seg=24):
    """Uniform Catmull-Rom through control points; returns dense polyline."""
    pts = [points[0]] + points + [points[-1]]
    dense = []
    for i in range(len(pts) - 3):
        p0, p1, p2, p3 = pts[i], pts[i + 1], pts[i + 2], pts[i + 3]
        for j in range(samples_per_seg):
            t = j / samples_per_seg
            t2, t3 = t * t, t * t * t
            x = 0.5 * ((2 * p1[0]) + (-p0[0] + p2[0]) * t +
                       (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 +
                       (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3)
            y = 0.5 * ((2 * p1[1]) + (-p0[1] + p2[1]) * t +
                       (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 +
                       (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)
            dense.append((x, y))
    dense.append(points[-1])
    return dense


def resample_arclength(dense, step=2.0):
    """Resample polyline to uniform arc-length steps; returns
    [(x, y, dist, dir_x, dir_y, curvature)] with curvature from the
    circumscribed circle of neighbours (1/radius, signed by turn)."""
    out = []
    prev = dense[0]
    acc = 0.0
    emit = [dense[0]]
    for p in dense[1:]:
        d = math.hypot(p[0] - prev[0], p[1] - prev[1])
        while acc + d >= step:
            f = (step - acc) / d
            np_ = (prev[0] + (p[0] - prev[0]) * f, prev[1] + (p[1] - prev[1]) * f)
            emit.append(np_)
            d = math.hypot(p[0] - np_[0], p[1] - np_[1])
            prev = np_
            acc = 0.0
        acc += d
        prev = p
    # directions + curvature
    for i, p in enumerate(emit):
        pa = emit[max(0, i - 1)]
        pb = emit[min(len(emit) - 1, i + 1)]
        dx, dy = pb[0] - pa[0], pb[1] - pa[1]
        L = math.hypot(dx, dy) or 1.0
        dx, dy = dx / L, dy / L
        curv = 0.0
        if 0 < i < len(emit) - 1:
            # signed curvature via cross of successive direction vectors
            pa2 = emit[i - 1]
            p0 = emit[i]
            p1 = emit[i + 1]
            v1 = (p0[0] - pa2[0], p0[1] - pa2[1])
            v2 = (p1[0] - p0[0], p1[1] - p0[1])
            l1 = math.hypot(*v1) or 1.0
            l2 = math.hypot(*v2) or 1.0
            cross = (v1[0] * v2[1] - v1[1] * v2[0]) / (l1 * l2)
            # heading change per metre (sign: + = left in x-east/y-north)
            ang = math.asin(max(-1.0, min(1.0, cross)))
            curv = ang / step
        out.append((p[0], p[1], i * step, dx, dy, curv))
    return out


SURFACES = {"bitumen": (0, 1.00), "gravel": (1, 0.72), "sand": (2, 0.55),
            "grass": (3, 0.50), "mud": (4, 0.42), "water": (5, 0.35)}
SURF_ID = {k: v[0] for k, v in SURFACES.items()}
SURF_MU = {k: v[1] for k, v in SURFACES.items()}

# ── M4 tilemap ──────────────────────────────────────────────────────────────
# Terrain height levels for Wang corner logic (lower renders first).
TERRAIN_LEVEL = {SURF_ID["water"]: 0, SURF_ID["sand"]: 1,
                 SURF_ID["grass"]: 2, SURF_ID["gravel"]: 3}
# Road corridor cells render as their path overlay; their base is grass.
WANG_SETS = ["wang_water_sand", "wang_sand_grass", "wang_grass_gravel"]
# road is procedural (RPG path tiles don't fit a multi-cell corridor):
# speckled fill per road surface + dithered soft edge on verge cells
ROAD_FILL = {SURF_ID["bitumen"]: "roadfill_bitumen",
             SURF_ID["gravel"]: "roadfill_gravel",
             SURF_ID["sand"]: "roadfill_sand"}
ROAD_DITHER = {SURF_ID["bitumen"]: "dither_bitumen",
               SURF_ID["gravel"]: "dither_gravel",
               SURF_ID["sand"]: "dither_sand"}
SECTION_ORDER = WANG_SETS + ["proc"]
# fill tile for a uniform level: all-lower of the set above it (or all-lower
# of the first set for water) — chained sets share fill art at boundaries.
FILL_KEY = {0: ("wang_water_sand", "wang:0,0,0,0"),
            1: ("wang_water_sand", "wang:1,1,1,1"),
            2: ("wang_sand_grass", "wang:1,1,1,1"),
            3: ("wang_grass_gravel", "wang:1,1,1,1")}
NO_TILE = 0xFFFF
TILEMAP_SLOTS = 5   # base + up to 3 wang layers + road overlay

PROP_TYPES = ["gum_tree", "gum_tree_tall", "paperbark", "dead_tree",
              "bush_round", "bush_wide", "grass_tuft", "spinifex",
              "boulder", "rock_cluster", "termite", "fence",
              "sign_arrow", "sign_caution", "hay_bale", "drum",
              "water_tank", "shed", "picnic_table", "fence_line",
              "driftwood", "dune_grass", "tyre_stack", "cone"]


class Atlas:
    """atlas.json reader: key -> global tile index per section."""

    def __init__(self, assets_dir):
        with open(os.path.join(assets_dir, "atlas.json")) as f:
            doc = json.load(f)
        self.base = {}
        self.count = {}
        self.idx = {}
        base = 0
        for name in SECTION_ORDER:
            if name not in doc:
                raise SystemExit(f"atlas.json missing section {name} "
                                 f"(run tilebake for it first)")
            entries = doc[name]["entries"]
            self.base[name] = base
            self.count[name] = len(entries)
            for e in entries:
                self.idx[(name, e["key"])] = base + e["index"]
            base += len(entries)
        self.total = base

    def tile(self, name, key):
        v = self.idx.get((name, key))
        if v is None:
            raise SystemExit(f"atlas has no tile {name}:{key}")
        return v

    def write_sections_h(self, path):
        with open(path, "w") as f:
            f.write("// generated by trackbake.py from assets/atlas.json — do not edit\n")
            f.write("#pragma once\n")
            f.write(f"#define TILE_SEC_COUNT {len(SECTION_ORDER)}\n")
            names = ", ".join('"%s"' % s for s in SECTION_ORDER)
            f.write(f"static const char *const TILE_SEC_NAMES[TILE_SEC_COUNT] = {{{names}}};\n")
            bases = ", ".join(str(self.base[s]) for s in SECTION_ORDER)
            f.write(f"static const int TILE_SEC_BASES[TILE_SEC_COUNT] = {{{bases}}};\n")
            sizes = ", ".join(str(self.count[s]) for s in SECTION_ORDER)
            f.write(f"static const int TILE_SEC_SIZES[TILE_SEC_COUNT] = {{{sizes}}};\n")
            f.write(f"#define TILE_GRASS_FILL {self.tile('wang_sand_grass', 'wang:1,1,1,1')}\n")


def _fnv(seed, *vals):
    h = 2166136261 ^ seed
    for v in vals:
        for shift in (0, 8, 16, 24):
            h ^= (v >> shift) & 0xFF
            h = (h * 16777619) & 0xFFFFFFFF
    return h


def compute_tilemap(grid, corridor, gw, gh, atlas, seed):
    """Per cell: [base, layer1, layer2, NO_TILE|layer3, road_overlay]."""
    def terrain_at(cx, cy):
        cx = min(max(cx, 0), gw - 1)
        cy = min(max(cy, 0), gh - 1)
        i = cy * gw + cx
        if corridor[i]:
            # the creek splash is corridor AND water: render it as water
            # terrain (no road overlay) so the crossing reads as a splash
            if grid[i] == SURF_ID["water"]:
                return 0
            return 2  # road corridor base is grass under the overlay
        return TERRAIN_LEVEL.get(grid[i], 2)

    tmap = []
    for cy in range(gh):
        for cx in range(gw):
            i = cy * gw + cx
            corners = [terrain_at(cx, cy), terrain_at(cx + 1, cy),
                       terrain_at(cx, cy + 1), terrain_at(cx + 1, cy + 1)]
            lo, hi = min(corners), max(corners)
            slots = []
            slots.append(atlas.tile(*FILL_KEY[lo]))
            for L in range(lo + 1, hi + 1):
                bits = [1 if t >= L else 0 for t in corners]
                key = "wang:%d,%d,%d,%d" % tuple(bits)
                slots.append(atlas.tile(WANG_SETS[L - 1], key))
            while len(slots) < TILEMAP_SLOTS - 1:
                slots.append(NO_TILE)
            # road overlay: speckled fill on the corridor, dithered soft
            # edge on verge cells adjacent to it
            ov = NO_TILE
            if corridor[i] and grid[i] != SURF_ID["water"]:
                ov = atlas.tile("proc", ROAD_FILL[grid[i]])
            elif not corridor[i]:
                neigh = []
                if cy > 0: neigh.append(i - gw)
                if cx + 1 < gw: neigh.append(i + 1)
                if cy + 1 < gh: neigh.append(i + gw)
                if cx > 0: neigh.append(i - 1)
                for ni in neigh:
                    if corridor[ni] and grid[ni] in ROAD_DITHER:
                        ov = atlas.tile("proc", ROAD_DITHER[grid[ni]])
                        break
            slots.append(ov)
            tmap.append(slots)
    return tmap


def scatter_props(grid, corridor, gw, gh, ox, oy, cell, seed):
    """Deterministic prop placement on non-corridor grass/sand cells."""
    # keep-clear halo: 2 cells around the corridor
    near = bytearray(gw * gh)
    for cy in range(gh):
        for cx in range(gw):
            if not corridor[cy * gw + cx]:
                continue
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    x, y = cx + dx, cy + dy
                    if 0 <= x < gw and 0 <= y < gh:
                        near[y * gw + x] = 1
    GRASS_PICK = [(0, 9), (1, 9), (2, 8), (3, 6), (4, 13), (5, 12),
                  (6, 9), (7, 9), (8, 6), (9, 6), (10, 5), (11, 4), (15, 4)]
    SAND_PICK = [(21, 40), (20, 20), (6, 20), (8, 10), (9, 10)]
    props = []
    occupied = set()
    for cy in range(gh):
        for cx in range(gw):
            i = cy * gw + cx
            if near[i]:
                continue
            sid = grid[i]
            pick = None
            if sid == SURF_ID["grass"]:
                if _fnv(seed, cx, cy, 1) % 100 < 8:
                    pick = GRASS_PICK
            elif sid == SURF_ID["sand"]:
                if _fnv(seed, cx, cy, 2) % 100 < 4:
                    pick = SAND_PICK
            if not pick:
                continue
            if any((cx + dx, cy + dy) in occupied
                   for dx in (-1, 0, 1) for dy in (-1, 0, 1)):
                continue
            roll = _fnv(seed, cx, cy, 3) % 100
            acc = 0
            ptype = pick[-1][0]
            for t, wgt in pick:
                acc += wgt
                if roll < acc:
                    ptype = t
                    break
            occupied.add((cx, cy))
            wx = ox + (cx + 0.5) * cell
            wy = oy + (cy + 0.5) * cell
            props.append((int(wx * 10), int(wy * 10), ptype))
            if len(props) >= 512:
                return props
    return props


def assign_surfaces(line, nodes):
    """Per-line-point surface + half-width, from the nearest node span.
    Nodes flagged water_splash override a ±6 m window to water."""
    # node arc positions (approximate by nearest control-point span)
    # simpler: march line points and node spans in parallel by distance
    node_pos = [(n["x"], n["y"], n["width"], n["surface"], n["flags"]) for n in nodes]
    # compute node dists along the dense line (project by nearest point)
    nd = []
    li = 0
    for (nx, ny, w, s, f) in node_pos:
        best_j, best_d = li, 1e18
        for j in range(li, len(line)):
            d = (line[j][0] - nx) ** 2 + (line[j][1] - ny) ** 2
            if d < best_d:
                best_d, best_j = d, j
        nd.append(best_j)
        li = best_j
    surf, halfw, flags = [], [], []
    for i in range(len(line)):
        # find span
        k = 0
        while k + 1 < len(nd) and nd[k + 1] <= i:
            k += 1
        k2 = min(k + 1, len(node_pos) - 1)
        i0, i1 = nd[k], nd[k2]
        f = 0.0 if i1 == i0 else (i - i0) / (i1 - i0)
        w = node_pos[k][2] * (1 - f) + node_pos[k2][2] * f
        s = node_pos[k][3] if f < 0.5 else node_pos[k2][3]
        fl = node_pos[k][4] | node_pos[k2][4]
        surf.append(s)
        halfw.append(w * 0.5)
        flags.append(fl)
    # water_splash overrides: ±3 line points (±6 m) around the flagged node
    for k, n in enumerate(nodes):
        if "water_splash" in n["flags"]:
            for i in range(max(0, nd[k] - 3), min(len(line), nd[k] + 4)):
                surf[i] = "water"
    return surf, halfw, flags


def target_speeds(line, surf):
    """v = sqrt(mu * g * radius) with a pre-braking pass (backward sweep)."""
    v = []
    for (x, y, d, dx, dy, curv), s in zip(line, surf):
        mu = SURF_MU[s]
        if abs(curv) < 1e-4:
            v.append(99.0)
        else:
            r = 1.0 / abs(curv)
            v.append(math.sqrt(mu * 9.81 * r))
    # clamp to stage max and sweep backward so braking is feasible
    vmax = 42.0
    v = [min(x, vmax) for x in v]
    a_brake = 6.0
    for i in range(len(v) - 2, -1, -1):
        step = line[i + 1][2] - line[i][2]
        allowed = math.sqrt(v[i + 1] ** 2 + 2 * a_brake * step)
        if v[i] > allowed:
            v[i] = allowed
    return v


def pacenotes(line, surf, flags):
    """Corners: contiguous curvature > threshold. Severity 1 (fastest) .. 6."""
    notes = []
    i = 0
    N = len(line)
    while i < N:
        if abs(line[i][5]) > 0.008:   # ~125 m radius
            j = i
            peak = 0.0
            sign = 1.0
            while j < N and abs(line[j][5]) > 0.004:
                if abs(line[j][5]) > peak:
                    peak = abs(line[j][5])
                    sign = 1.0 if line[j][5] > 0 else -1.0
                j += 1
            r = 1.0 / peak if peak > 1e-6 else 9999.0
            if r > 65:
                sev = 1
            elif r > 45:
                sev = 2
            elif r > 30:
                sev = 3
            elif r > 18:
                sev = 4
            elif r > 10:
                sev = 5
            else:
                sev = 6
            # tightens / opens: curvature trend through the corner
            head = sum(abs(line[k][5]) for k in range(i, (i + j) // 2))
            tail = sum(abs(line[k][5]) for k in range((i + j) // 2, j))
            trend = ""
            if tail > head * 1.5:
                trend = " tightens"
            elif head > tail * 1.5:
                trend = " opens"
            fl = set()
            for k in range(i, j):
                fl |= flags[k]
            extra = ""
            if "crest" in fl:
                extra += " over crest"
            if "jump" in fl:
                extra += " jump"
            direction = "left" if sign > 0 else "right"
            dist = line[i][2]
            txt = f"{direction} {sev}{extra}{trend}"
            notes.append((dist, txt))
            i = j
        else:
            i += 1
    # water splash: one note per contiguous flagged region
    k = 0
    while k < len(flags):
        if "water_splash" in flags[k]:
            notes.append((line[k][2], "caution water splash"))
            while k < len(flags) and "water_splash" in flags[k]:
                k += 1
        else:
            k += 1
    notes.sort(key=lambda n: n[0])
    # merge notes closer than 60 m apart (keep the first); cautions exempt
    merged = []
    for n in notes:
        if (merged and not n[1].startswith("caution")
                and not merged[-1][1].startswith("caution")
                and n[0] - merged[-1][0] < 60.0):
            continue
        merged.append(n)
    return merged


def checkpoints(line, v, every_s=8.0):
    """A checkpoint every ~every_s seconds along the racing line."""
    cps = []
    t = 0.0
    next_t = every_s
    for i in range(1, len(line)):
        step = line[i][2] - line[i - 1][2]
        sp = max(v[i], 2.0)
        t += step / sp
        if t >= next_t:
            cps.append(line[i][2])
            next_t += every_s
    if line[-1][2] - (cps[-1] if cps else 0) > 100.0:
        pass
    return cps


def bake(meta, nodes, out_path, assets_dir=None):
    pts = [(n["x"], n["y"]) for n in nodes]
    dense = catmull_rom(pts)
    line = resample_arclength(dense, step=2.0)
    surf, halfw, flags = assign_surfaces(line, nodes)
    v = target_speeds(line, surf)
    notes = pacenotes(line, surf, flags)
    cps = checkpoints(line, v)

    # surface grid: 4 m cells over the stage bbox + margin
    xs = [l[0] for l in line]
    ys = [l[1] for l in line]
    margin = 80.0
    ox, oy = min(xs) - margin, min(ys) - margin
    gx, gy = max(xs) + margin, max(ys) + margin
    cell = 4.0
    gw = int((gx - ox) / cell) + 1
    gh = int((gy - oy) / cell) + 1
    grid = bytearray([SURF_ID["grass"]] * (gw * gh))
    corridor = bytearray(gw * gh)

    # rasterise: default grass; road corridor = line surface; water at splash
    # distance-to-line via brute force per cell inside corridor bbox only
    li = 0
    # walk cells row by row with a coarse nearest-point sweep (O(N*M) is fine
    # offline: ~600x300 cells x 1100 line points)
    for cy in range(gh):
        wy = oy + (cy + 0.5) * cell
        for cx in range(gw):
            wx = ox + (cx + 0.5) * cell
            best = 1e18
            bi = 0
            for i in range(len(line)):
                dx = line[i][0] - wx
                dy = line[i][1] - wy
                d2 = dx * dx + dy * dy
                if d2 < best:
                    best = d2
                    bi = i
            d = math.sqrt(best)
            hw = halfw[bi]
            if d <= hw:
                grid[cy * gw + cx] = SURF_ID[surf[bi]]
                corridor[cy * gw + cx] = 1
            elif d <= hw + 4.0:
                # verge: sand country spreads; everything else stays grass
                # (the road overlay itself carries the gravel/bitumen read)
                if surf[bi] == "sand":
                    grid[cy * gw + cx] = SURF_ID["sand"]
            # else stays grass

    tilemap = None
    props = []
    atlas = None
    if assets_dir:
        atlas = Atlas(assets_dir)
        seed = 20260726
        tilemap = compute_tilemap(grid, corridor, gw, gh, atlas, seed)
        props = scatter_props(grid, corridor, gw, gh, ox, oy, cell, seed)
        atlas.write_sections_h(os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "core",
            "tiles_sections.h"))

    # blob (all records 4-byte aligned for direct-cast parsing on ARM)
    version = 2 if tilemap else 1
    out = bytearray()
    out += b"RSTG"
    out += struct.pack("<HHHHHH", version, len(nodes), len(line),
                       len(notes), len(cps), gw)
    out += struct.pack("<HH", gh, len(props))
    out += struct.pack("<fff", ox, oy, cell)
    for n in nodes:
        fl = 0
        for name, bit in (("jump", 1), ("crest", 2), ("narrows", 4),
                          ("hairpin", 8), ("caution", 16), ("water_splash", 32)):
            if name in n["flags"]:
                fl |= bit
        out += struct.pack("<fffBBxx", n["x"], n["y"], n["width"],
                           SURF_ID[n["surface"]], fl)
    for i, p in enumerate(line):
        fl = 0
        for name, bit in (("jump", 1), ("crest", 2), ("narrows", 4),
                          ("hairpin", 8), ("caution", 16), ("water_splash", 32)):
            if name in flags[i]:
                fl |= bit
        out += struct.pack("<ffffBBxx", p[0], p[1], v[i], halfw[i],
                           SURF_ID[surf[i]], fl)
    for dist, txt in notes:
        b = txt.encode()[:30]
        out += struct.pack("<fB", dist, len(b)) + b + b"\0" * (31 - len(b))
    for c in cps:
        out += struct.pack("<f", c)
    out += bytes(grid)
    if tilemap:
        for slots in tilemap:
            out += struct.pack("<5H", *slots)
        for x_dm, y_dm, ptype in props:
            out += struct.pack("<iiB3x", x_dm, y_dm, ptype)
    with open(out_path, "wb") as f:
        f.write(out)
    return line, v, notes, cps, (ox, oy, cell, gw, gh), len(out), len(props)


def profile(meta, nodes):
    pts = [(n["x"], n["y"]) for n in nodes]
    dense = catmull_rom(pts)
    line = resample_arclength(dense, step=2.0)
    surf, halfw, flags = assign_surfaces(line, nodes)
    v = target_speeds(line, surf)
    notes = pacenotes(line, surf, flags)
    cps = checkpoints(line, v)
    total = line[-1][2]
    t_est = 0.0
    for i in range(1, len(line)):
        t_est += (line[i][2] - line[i - 1][2]) / max(v[i], 2.0)
    print(f"stage: {meta.get('name')}")
    print(f"nodes: {len(nodes)}  line points: {len(line)}  length: {total:.0f} m")
    print(f"est time: {t_est:.1f} s  (target 100-120 s)")
    print(f"checkpoints: {len(cps)}  pacenotes: {len(notes)}")
    print("corners:")
    for d, txt in notes:
        print(f"  {d:7.0f} m  {txt}")
    # speed histogram
    slow = sum(1 for x in v if x < 8)
    mid = sum(1 for x in v if 8 <= x < 20)
    fast = sum(1 for x in v if x >= 20)
    print(f"speed profile: slow {slow*2} m / mid {mid*2} m / fast {fast*2} m")
    # surfaces
    from collections import Counter
    c = Counter(surf)
    print("surface metres:", {k: vv * 2 for k, vv in sorted(c.items())})


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    mode, toml = sys.argv[1], sys.argv[2]
    meta, nodes = read_toml_nodes(toml)
    if mode == "profile":
        profile(meta, nodes)
    elif mode == "bake":
        assets = sys.argv[4] if len(sys.argv) > 4 else None
        line, v, notes, cps, gridinfo, nbytes, nprops = bake(
            meta, nodes, sys.argv[3], assets)
        print(f"baked {sys.argv[3]}: {nbytes} bytes, "
              f"{len(line)} line pts, {len(notes)} notes, {len(cps)} cps, "
              f"grid {gridinfo[3]}x{gridinfo[4]} @ {gridinfo[2]} m, "
              f"{nprops} props")
    else:
        print(f"unknown mode {mode}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
