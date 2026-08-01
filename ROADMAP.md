# PicOS Rally — Roadmap

Status as of 2026-08-01. **M0–M5 are done** (measured platform facts → grey
box → physics → track → assets/renderer → feel). The game runs on hardware:
Cooloola Point completable, real art, synth audio, 30 fps locked.

---

## M6 — Polish (next, closes 1.0)

### Ghost
Record the best run's inputs per sim step (deterministic replay — the sim is
hash-verified, so inputs are sufficient; no car-state snapshot needed).
Persist to `/data/com.picos.rally/ghost.bin` via the fs API. Replay as a
translucent ghost car on subsequent runs (CLUT-shade the sprite), with a
HUD delta readout (±s vs ghost at each checkpoint). Best run replaces ghost
only when the finish time beats it.

### Settings
Persist via `appconfig` (`/data/com.picos.rally/config.json`):
- assist dial (default 0.60 — **currently untested with a human**; expose
  0.0/0.3/0.6/0.9 steps)
- audio volume (master, engine/sfx balance if the ear test demands it)
- debug overlay default off (currently defaults on)
- pace lock (30 cap / uncapped, for measurement)

### Tuning pass
One more drive-feel iteration with real art + audio: steering ramps, lock
curve, handbrake kick, traction limit feel on gravel launch. The hot-reload
loop ('r' key) makes this a ~5 s cycle on device. The polishing run below
did the numbers pass (item 3); what's left is a human drive to confirm the
handbrake kick and gravel-launch feel at the new pace.

### Polishing run (drive feedback, 2026-08-01) — **done, owes a drive**

All five items are implemented and verified host-side (52/52 headless
checks, 15/15 bundle checks, ARM build clean). Numbers below are measured
with `plat/headless/perf`, a new drive-feel harness that runs the real sim.

1. **Wider track** — per-node widths up ~30% (9.0→12.0, 8.0→10.5, 7.0→9.0,
   6.5→8.5), keeping the township tighter than the flats. Pacenotes are
   unchanged (21 — severity comes from centreline curvature, not width) and
   the hairpins still resolve without the corridor blobbing into itself.
2. **More trees/rocks near the track** — `scatter_props()` now takes
   candidates nearest the corridor first, in three density bands (34/16/7%),
   with gums, paperbarks and boulders weighted into the near band and the
   keep-clear halo cut from 2 cells to 1 so canopy just overhangs the verge.
   Cap 512→1500. **This was hiding a bug:** the old scan was row-major and
   stopped at the cap, so all 512 props landed in y −103..−43 of a −105..655
   map and the entire stage drove past bare verges.
3. **Responsiveness** — 0–100 km/h **10.73 s → 6.50 s**, 100–0 braking
   **48.6 m → 38.6 m**, top speed 124 → 166 km/h, turn-in to 90° at 54 km/h
   3.45 s → 3.38 s (peak yaw 0.55 → 0.58 rad/s). The turning lever was not
   the obvious one: the front axle saturates at `mu*load`, so `ca_front` has
   *no* effect and extra lock only scrubs (17° turns slower than 14°).
   Winding lock off earlier (`curve_knee` 0.60→0.45) is what sharpened it.
4. **Car sprite breaks up when rotating** — root cause was the source art,
   not the bake: the 8 PixelLab views were generated at a side/low camera,
   so apparent elevation swung per heading and the car's drawn length went
   49→61→52 px across the set when a rigid body must hold constant. Replaced
   with a single true top-down view (`art/png/car_topdown.png`, PixelLab
   pixflux at 128², car-only palette subset) rotated through all 32 headings.
   `spritebake.py` gained `--fit` (centre on the rotation pivot in a square
   the size of the content diagonal, so no heading clips its corners) and
   `--snap-subset` (palette snapping restricted to colours the source uses,
   so downscale blends can't land on gravel brown).
5. **Smoother road edges** — edge cells now store the corridor boundary
   itself as a quantised (angle, offset) half-plane cut, derived
   analytically from the nearest racing-line point, with a Bayer band
   scattering a few pixels of gravel along it. Marching squares over cell
   corners was tried first and is *not* enough: a ~2-cell-wide corridor
   often has one corner inside on a diagonal and breaks into blobs (419
   such cells). Purely a bake change — the renderer already masked-blits
   the overlay slot.

Two renderer/bake bugs fell out of this and are fixed:

- **`gfx_t.gtile` was 256 entries and blits masked the index with `& 0xFF`**,
  so any atlas over 256 tiles silently drew *wrong* art rather than failing
  (it put terrain tiles on the bitumen road). Tilemap slots have always been
  u16, so the format was never the limit. Now `GFX_MAX_TILES` (1024),
  out-of-range draws nothing, and `trackbake` refuses to bake past it.
- **`_fnv` did not avalanche.** Callers draw several independent decisions
  per cell by varying only the last value (density tag 1, prop type tag 3);
  plain FNV-1a left them dependent, and on stage01 type rolls 60..79 were
  *unreachable* for any cell the density pass had picked — silently deleting
  whole prop types. Fixed with a murmur3 finalizer.

Still owed: a human drive at the new pace (the assist dial is still
untested with a human), and a hardware frame-time check — the prop count
tripled and the proc tile set went 6 → 627 tiles (`tiles_proc.bin` 160 KB;
assets total 1.32 MB against the 5.86 MB PSRAM budget).

Note the stage now estimates 94.2 s against the 100–120 s design target
(the AI drive is 136.8 s). If that matters, lengthen the stage rather than
slowing the car back down.

### Readability pass
Drive the whole stage at pace and fix every corner that reads late at
320×240: pacenote lead times, chevron contrast, sign props at sev 4+
corners, crest landing visibility, beach section contrast.

---

## Owed human verification (hardware)

- **Ear test (M5)**: engine pitch vs speed, gravel roar level, event
  balance (beeps/split/finish/splash/thud), overall volume.
- **M4 visual checks**: gravel hue (reads salmon-ish on the LCD?), sand
  road vs beach terrain contrast.
- **M6 polishing run**: drive it — acceleration/braking/turn-in at the new
  tune, and whether the assist dial still suits the higher pace. Also a
  frame-time check: props 512 → 1500 and proc tiles 6 → 627.

---

## Deferred from earlier milestones

- **PixelLab 8px font** (`rally-hud`, already generated): glyph atlas →
  HUD integration. Currently everything is the firmware 6×8 (scaled).
- **HUD panel art**: the PixelLab panel could back the intro/results
  plates (they're flat palette fills now).
- **Ruts**: directional rut overlay along the racing line (dark wheel
  tracks baked into the tilemap pass), not runtime tiles.
- **Grass fill variants**: 2–3 art variants to break base-terrain
  uniformity (hash-picked per cell at bake).
- **Per-wheel surface lookup**: physics currently samples per axle.
- **60 fps**: needs DMA below 16.6 ms/frame (currently 17.6: 13.07
  viewport + 4.52 HUD strip). Options: 224-row viewport, HUD flush only on
  change, or accepting 50 fps. Not scheduled.

---

## PicOS-side (upstream — lives in the PicOS repo, tracked here)

- Port the sim flushRows/flushRegion dirty-buffer fix (`72f7576a` on
  feat/picos-rally) to develop.
- `drawPlane` host-order texture documentation fix (one-liner).
- `MAX_APPS=32` silently drops the 33rd app — hit during rally dev with a
  full SD card.

---

## Post-1.0 ideas (unscheduled)

- Stage 2 (the pipeline supports it: new TOML → rebake; terrain chain and
  props already generic).
- SD-persisted leaderboard (top 10 stage times).
- Car palette swaps (CLUT section swap — the 8bpp pipeline makes this
  nearly free).
- Co-driver voice beeps (distinct pitch per severity).

---

## Done (M0–M5 highlights)

- **M0**: push 13.07 ms viewport @200 MHz, 3-key rollover, 5.86 MB PSRAM
  budget, no overclock needed.
- **M1**: §4a decided ORTHO over Mode 7 (measured); sim step 0.15 ms.
- **M2**: bicycle model + per-axle surfaces + traction limit; tuning
  locked with human drives.
- **M3**: Cooloola Point 2670 m + trackbake + completability AI (139 s
  deterministic); race flow on hardware.
- **M4**: 48-colour palette lock, PixelLab batch, 8bpp tile renderer,
  blob v2 tilemap, 30 fps lock; procedural road after RPG path tiles
  failed (see NOTES.md §6).
- **M5**: Core-1 synth audio (pacing trap found+fixed: floor 16
  samples/call), dust/skids, countdown/GO/pacenote presentation.
