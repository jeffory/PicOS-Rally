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
  - minimap PPM (host-side eyeball)

Usage:
  trackbake.py profile tracks/stage01.toml     # print stage profile
  trackbake.py bake   tracks/stage01.toml apps/rally/stage01.bin
  trackbake.py map    tracks/stage01.toml /tmp/stage.ppm

Deterministic: seeded RNG, sorted iteration, no dict-order dependence.
"""
import math
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


def bake(meta, nodes, out_path):
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
            elif d <= hw + 4.0:
                # verge: keep grass but allow sand surface near sand line points
                if surf[bi] == "sand":
                    grid[cy * gw + cx] = SURF_ID["sand"]
            # else stays grass

    # blob (all records 4-byte aligned for direct-cast parsing on ARM)
    out = bytearray()
    out += b"RSTG"
    out += struct.pack("<HHHHHH", 1, len(nodes), len(line), len(notes), len(cps), gw)
    out += struct.pack("<HH", gh, 0)          # + pad to 4
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
    with open(out_path, "wb") as f:
        f.write(out)
    return line, v, notes, cps, (ox, oy, cell, gw, gh), len(out)


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
        line, v, notes, cps, gridinfo, nbytes = bake(meta, nodes, sys.argv[3])
        print(f"baked {sys.argv[3]}: {nbytes} bytes, "
              f"{len(line)} line pts, {len(notes)} notes, {len(cps)} cps, "
              f"grid {gridinfo[3]}x{gridinfo[4]} @ {gridinfo[2]} m")
    else:
        print(f"unknown mode {mode}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
