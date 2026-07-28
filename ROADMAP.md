# PicOS Rally — Roadmap

Status as of 2026-07-28. **M0–M5 are done** (measured platform facts → grey
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
loop ('r' key) makes this a ~5 s cycle on device.

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
