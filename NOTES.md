# PicOS Rally — NOTES.md

Milestone 0 findings first; tuning observations and hardware findings accrete here.
This file is the artefact that survives the project. Date format: 2026-07-26.

---

## 0. Milestone 0 — measured platform facts

### 0.1 App model (from `apps/doom` + `src/os/native_loader.c`)

- A native app is a directory `/apps/<name>/` on the SD card: `app.json` +
  `main.elf` (ELF32 PIE, ARM Thumb-2). Native wins over `main.lua` if both exist.
- Build: `arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -fpie -fno-plt
  -ffunction-sections -fdata-sections -T linker.ld -Wl,--entry=picos_main -Wl,-pie
  -nostartfiles -nodefaultlibs -lc -lm -lgcc` (doom Makefile). Link against
  `sdk/native/os.h` + `app_abi.h` only — no Pico SDK headers; the OS owns hardware.
- Entry: `void picos_main(const PicoCalcAPI *api, const char *app_dir,
  const char *app_id, const char *app_name)`. Returning = exit to launcher.
- Lifecycle loop (doom `dg_picos.c`): `while (!api->sys->shouldExit()) {
  api->sys->poll(); …tick… }`. `poll()` pumps keyboard, HTTP callbacks, and the
  Sym-key system menu; it also feeds the 10 s watchdog (core1 relays a heartbeat;
  >60 s without a poll reboots the device — feed it during long loads).
- Loader: ELF PT_LOADs into PSRAM (`umm_malloc`, cached alias 0x11xxxxxx, written
  via uncached alias + range-invalidate). Code segment ≤16 KB may split into SRAM.
  App runs on PSP with a dedicated stack (16 KB SRAM preferred, 64 KB PSRAM
  fallback), canary-guarded. Max image 7 MB; practical PSRAM ceiling for the app
  image + its `qmiAlloc` heap is ~5–6 MB (firmware + Lua heap share the 8 MB).
- Exit paths: return from `picos_main`, or `exit()` longjmp (doom pattern).
  `shouldExit()` fires once when the user picks "Exit App" in the system menu.
- `app.json` keys that matter: `id`, `name`, `type:"native"`,
  `system_clock_khz` (doom ships 300000), `requirements` (e.g. `["audio"]`,
  `"root-filesystem"`), `min_firmware`, `category`.
- **Launcher caches the app list at boot** — newly staged apps need a device/sim
  restart. `MAX_APPS = 32` and the 33rd is silently dropped (known trap).

### 0.2 Simulator loop (validated 2026-07-26)

The picos MCP drives everything. Exact sequence that works:

```
# one-time per worktree: build the sim
cd /home/keith/Projects/picos-rally && make simulator        # → build_sim/picos_simulator

# build a native app (example: SDK hello)
cd sdk/native && make                                        # → main.elf

# stage: app.json + main.elf in a dir, then via MCP:
start_simulator(project_root=/home/keith/Projects/picos-rally, headless=true)
get_status()                    # MUST show "SimulatorWiFi" — else you're on hardware!
push_app(local_dir=<stage dir>, app_name=hello_c)   # copies into <worktree>/apps/
# launcher caches apps at boot → restart sim when a NEW app appears:
shutdown_simulator(); start_simulator(...)
list_apps() / launch_app("hello_c")
screenshot(save_path=...)       # pixel ground truth
keypress("enter")               # or "down 5x", "ctrl+s" chords
wait_for_exit("hello_c", timeout=10)
get_log_buffer(lines=N)         # [APP]/[DEV] stdout
```

Notes:
- With `project_root` set, the sim mounts `<worktree>/apps/` as `/apps` on its SD —
  `push_app` lands straight in the source tree (untracked; remember to clean or commit).
- Iterating on an EXISTING app needs no sim restart — re-push + relaunch.
- Sim divergences from hardware (from CLAUDE.md/memory, kept current below in §0.8):
  no crypto, no real rollover/timing, launcher app-list cache, GFXSTAT depends on
  staged-SD readdir order, LUAL_BUFFERSIZE 512 vs 256, sim display is host-endian.

### 0.3 API signatures (verbatim from `sdk/native/os.h`, g_api.version = 5)

Only the subsets Rally needs. Full table in `sdk/native/os.h`.

```c
// input
uint32_t (*getButtons)(void);          // held bitmask (BTN_UP/DOWN/LEFT/RIGHT/ENTER/ESC/F1-9/BACKSPACE/TAB/DEL/SHIFT/CTRL/ALT/FN)
uint32_t (*getButtonsPressed)(void);   // rising edge this poll
uint32_t (*getButtonsReleased)(void);  // falling edge this poll
char     (*getChar)(void);             // last ASCII char (0 if none)

// display (320x320, RGB565; back buffer is BIG-ENDIAN — byte-swap direct writes)
void (*clear)(uint16_t rgb565);
void (*fillRect)(int x,int y,int w,int h,uint16_t);
int  (*drawText)(int x,int y,const char*,uint16_t fg,uint16_t bg);
void (*flush)(void);                   // full-frame, swap, non-blocking DMA
void (*flushRegion)(int y0,int y1);    // rows y0..y1, swap, non-blocking (+front→back sync memcpy)
void (*flushRows)(int y0,int y1);      // rows only, NO swap
uint16_t* (*getBackBuffer)(void);      // direct writes (big-endian!)
void (*setClipRect)(int x,int y,int w,int h);   // v4
void (*setScrollArea)(int top,int scroll_h,int bottom); // v4 ST7365P VSCRDEF
void (*setScrollOffset)(int offset);   // v4 hardware vertical scroll
void (*drawPlane)(const uint16_t *tex,int tw,int th,   // v4 Mode 7 ground plane
                  float cam_x,float cam_y,float cam_z,
                  float angle,int horizon_y,float scale);

// sys
uint32_t (*getTimeMs)(void);
uint64_t (*getTimeUs)(void);
void (*log)(const char *fmt, ...);
void (*poll)(void);                    // keyboard + HTTP cb + menu + watchdog feed
bool (*shouldExit)(void);
void (*setAudioCallback)(void (*cb)(void));  // called on CORE 1 every core1 tick
void (*addMenuItem)(const char*, void(*)(void*), void*);

// audio — PWM, stereo int16 interleaved; ring is 4096 frames of uint8
void (*startStream)(uint32_t sample_rate);
void (*stopStream)(void);
void (*pushSamples)(const int16_t *samples, int count); // drops when ring full
void (*setVolume)(uint8_t);

// fs (SD card, FatFS)
pcfile_t (*open)(const char*, const char*);
int  (*read)(pcfile_t, void*, int);  int (*write)(pcfile_t, const void*, int);
void (*close)(pcfile_t);  bool (*exists)(const char*);  int (*size)(const char*);
bool (*seek)(pcfile_t, uint32_t);  bool (*mkdir)(const char*);
int  (*listDir)(const char*, cb, void*);

// psram
void *(*qmiAlloc)(uint32_t size);   // = umm_malloc on the 8MB QMI heap
void  (*qmiFree)(void*);

// perf
void (*beginFrame)(void); void (*endFrame)(void);
int  (*getFPS)(void); uint32_t (*getFrameTime)(void);
void (*drawFPS)(int,int); void (*setTargetFPS)(uint32_t);

// zip (v5) — read-in-place archives: ship assets as ONE pack file
pczip_t (*open)(const char*); void (*close)(pczip_t);
int (*numEntries)(pczip_t); int (*locate)(pczip_t, const char*);
bool (*statIndex)(pczip_t,int,pczip_stat_t*);
int (*read)(pczip_t,int,void*,uint32_t cap);
```

### 0.4 Memory & hardware budget left to an app

| Resource | Available | Notes |
|---|---|---|
| SRAM heap (`malloc`) | ~28.8 KB after firmware BSS | firmware owns 2×204.8 KB framebuffers; loader takes 16 KB app stack from here when available → ~12 KB scratch. Tiny. |
| App stack | 16 KB SRAM (or 64 KB PSRAM) | PSP, canary-checked |
| QMI PSRAM (`qmiAlloc`/umm) | ~5–6 MB practical | app image + runtime heap; 8 MB total shared with firmware/Lua |
| PIO PSRAM (8 MB, `psram->pio*`) | mostly free | MP3 ring uses 32 KB @0x0000, video pool @0x8000; slower bus — bulk asset store candidate |
| Flash | n/a (apps live on SD) | ELF ≤ 7 MB image; assets on FAT32 |
| Core 1 | **reserved by firmware** | network + audio decode + app `setAudioCallback` hook at core1 tick (1 kHz timer) |
| Display DMA | firmware-owned, free to use | non-blocking; overlaps with app CPU by design |

Consequence: the spec's 8bpp viewport framebuffer + tilemap + world data live in
**PSRAM**, not SRAM. The "second viewport buffer" the spec budgets in SRAM is
unnecessary — the firmware's double buffer already provides render-vs-DMA overlap.

### 0.5 Display path (driver facts; hardware measurement pending → §0.7)

- ST7365P 320×320 over **PIO0 SPI @ 100 MHz** (`LCD_SPI_BAUD`; PIO divider is
  `clk_sys/(2·int_div)` rounded up → at 200 MHz sysclk = 100 MHz; at **300 MHz the
  integer divider drops SPI to 75 MHz** — overclocking costs display bandwidth).
- Pixels are RGB565 = **2 bytes/px** on the wire. Theoretical wire times:
  full frame 204800 B ≈ **16.4 ms** (~61 fps ceiling);
  320×240 viewport 153600 B ≈ **12.3 ms**; HUD strip 320×80 ≈ 4.1 ms.
- `flush()`/`flushRegion()` are non-blocking DMA + buffer swap; DMA reads SRAM on
  AHB and does not contend with app code in PSRAM (XIP). `g_display_flush_blocking`
  exists but is OFF (native apps no longer block on flush).
- Render pattern (doom): game converts 8bpp indexed → RGB565-BE through a LUT
  straight into `getBackBuffer()`, then `flushRegion(y0,y1)`. 320×200 conversion
  costs doom ~1 ms-class CPU. This is the rally render path.
- `flushRegion` does a front→back sync memcpy for the flushed rows (so the other
  buffer stays coherent). A `_nocopy` variant exists in firmware but is NOT in the
  app API — flag if the memcpy shows up in the frame profile.
- Mode 7: `display_draw_plane` (firmware, v4 API) — per-scanline perspective,
  16.16 fixed-point inner loop, RGB565 texture, pow2 wraps / else clamps, writes
  byte-swapped into back buffer, respects clip rect. ~5 ms class for a 240-line
  plane at 200 MHz if texture reads behave (PSRAM/XIP locality per row is the open
  question). Texture must be `uint16_t` RGB565 host-order, linear bitmap.
  **Builds and is wired into both Lua (`picocalc.display.drawPlane`) and native.**
  M1 spike: grey-box car + steep pitch + scrolling plane, measure on hardware.
- Stale comment trap: `sdk/native/main.c` says "~65 ms for 320x320 at 25 MHz" and
  "Core 1 WiFi races the SPI pins" — pre-PIO-display history. Current: 100 MHz PIO,
  independent buses, ~16.4 ms.

### 0.6 Input path (driver facts; rollover test pending → §0.7)

- STM32F103 keyboard MCU over I2C1 @ **10 kHz**. 100 kHz is documented to lock up
  the STM32 after extended use (needs power cycle) — **do not raise the baud.**
- Protocol is an **event FIFO** (REG_FIF 0x09): up to 8 `[state,keycode]` events
  per poll, states PRESSED/HOLD/RELEASED. Held state is accumulated in software —
  **the protocol represents arbitrary key chords natively**; whether the STM32
  matrix scanner reports 3 simultaneous gaming keys is a physical test (§0.7).
- **Poll rate is capped at 20 Hz** (`s_next_i2c_ms += 50`): each FIFO read costs
  ~5–6 ms at 10 kHz so per-frame polling would eat a third of a 60 fps frame.
  Events queue in the STM32 FIFO between polls and are drained in batches.
  ⇒ Worst-case input delivery latency ≈ 50 ms + STM32 scan latency + I2C drain.
  This is THE platform constraint for driving feel. Mitigations: 180 ms steering
  ramp + assist (already the design), fixed 60 Hz sim steps, and if measurement
  says it's bad, a PicOS-level change (e.g. adaptive poll rate while an app holds
  buttons, or a reduced FIFO-read cost) — to be proposed, not hacked around.
- BTN_MENU (F10) is intercepted by the driver for the OS; apps never see it.
  Idle-dim swallows the waking press. KEY_BRK is the screenshot trigger.
- `sys->poll()` throttled internally; calling it more than once per frame is safe
  (doom feeds the watchdog via `DG_GetTicksMs`).

### 0.7 Clocks

- Default 200 MHz. Apps request `system_clock_khz` in app.json; launcher retunes
  voltage (1.20 V @300), **QMI PSRAM timing pre-scale + exact retune**, PIO-PSRAM
  sysclk, clk_peri, and re-derives display/kbd/sdcard/uart clocks. 300 MHz is
  proven stable by doom (this retune is the fix for the old 300 MHz crashes).
- Cost of 300 MHz: LCD SPI drops 100→75 MHz (integer PIO divider) — full-frame
  wire time 16.4→21.8 ms, viewport 12.3→16.4 ms. CPU +50 %.
- 400 MHz = 1.25 V, uncharted; not proposing it.
- Pending: measured frame/push times on hardware (§0.9 table when filled).

### 0.8 Core 1 & audio

- Core 1 is firmware-owned: 1 kHz tick does wifi_poll, HTTP, `audio_stream_poll`,
  mp3/fileplayer/mod updates, **`g_native_audio_callback`** (the app hook),
  image preload, video prefetch. Apps never get core1 — they get this hook plus
  non-blocking DMA flush. That covers the spec's "core1 = display DMA + audio
  mixing" intent with zero new platform code.
- PCM streaming: `startStream(rate)` → `pushSamples(int16 stereo, frames)`;
  samples truncate to uint8 PWM (wrap 1699). Ring = 4096 frames (~93 ms @44.1k,
  ~186 ms @22k). **Ring full → samples dropped, never blocks** (spec requirement
  met by design). DMA ISR on core1 feeds PWM from the ring.
- Engine/surface synth plan: generate into the ring from the core1 callback
  (doom mixes SFX exactly this way via `setAudioCallback`).

### 0.9 Hardware measurements (DONE 2026-07-26, hwbench probe, `probes/hwbench/`)

| Measurement | Prediction | Measured @200 MHz | Measured @300 MHz | Verdict |
|---|---|---|---|---|
| Full-frame push 320×320 | 16.4 ms | **17.420 ms** (57.4 fps cap) | **23.216 ms** (43.0 fps cap) | RGB565 2 B/px confirmed; ~6 % overhead over wire theory |
| Viewport push 320×240 | 12.3 ms | **13.068 ms** (76.5 fps cap) | **17.414 ms** (57.4 fps cap) | viewport-only @300 ≈ full-frame @200 |
| HUD strip push 320×80 | 4.1 ms | **4.517 ms** | **5.823 ms** | cheap when dirty-only |
| App PSRAM budget | ~5–6 MB | **6 139 000 B free** at launch (launcher log) | same | 5.86 MB — design to it |
| App stack | 16 KB SRAM pref | 64 KB PSRAM (SRAM heap didn't have 16 KB free) | same | PSRAM stack is the real-world default |
| Key rollover (physical) | protocol supports | **UP+LEFT+SHIFT = OK (3-key)**. +ENTER ghosts into phantom TAB; ENTER never registers | — | matrix ghosting past 3 keys. **Handbrake = SHIFT confirmed viable. Never design for 4 simultaneous.** |
| Poll interval | 20 Hz cap (code) | 20 Hz (code-confirmed; edges delivered in batches, none lost) | — | worst-case delivery ≈ 50 ms + scan + I2C ≈ 60 ms |
| `drawPlane` 240-line cost | ~5 ms class | (M1 spike) | (M1 spike) | |
| Sim step (2× bicycle) | ≤2 ms budget | (M1) | (M1) | |

Frame-rate conclusion (falls out of the measurement, per spec): **30 fps is
comfortable at 200 MHz** (13.1 ms viewport DMA overlaps sim+render inside a
33.3 ms frame) and **60 fps is plausibly reachable at 200 MHz** if sim+render
stay ≤16.6 ms — decide at M4 with the real renderer profile. 300 MHz buys CPU
at a direct display-rate cost (43 fps full-frame cap); only worth it if the sim
step or renderer needs the cycles — revisit after M1/M2 benchmarks.

Gotchas recorded for the project:
- Native `sys->log` lines print WITHOUT the `[APP]` prefix on hardware (Lua has
  the prefix). Grep for the raw message when reading hardware captures.
- newlib-nano: no `%f`, and `%llu` prints garbage — use 32-bit fixed-point.
- MCP `keypress esc` delivers char 27 via `getChar()`, NOT `BTN_ESC` — apps that
  only poll `getButtons()` will never see it (hello_c exits because it also
  checks `getChar()`). Physical ESC sets the button bit. Design Rally to accept
  both, or use `shouldExit()` (menu path) as the primary exit.
- Re-pushing an app that is RUNNING fails at unzip ("write failed") — exit first.
- Sim cannot measure flush time: every present is rate-paced to 60 Hz there
  (16.67 ms measured for all three bench variants — identical, i.e. paced, not wire).
- Clock retune on app start is visible and correct in the serial log
  (QMI div=3@200 → div=4@300, SCK 66.7→75 MHz; PIO PSRAM 33.3→50 MHz).

### 0.10 PixelLab

- MCP connected, subscription active (Tier 1), **1931 generations remaining**,
  $0.00 credits (subscription gens only). Pro mode (20–40 gen/direction) needs
  user approval per call — will ask before any such batch.

---

## 1. Reuse inventory (spec §2.1)

| Need | Provided by | How Rally uses it |
|---|---|---|
| Display | **PicOS** | `getBackBuffer` + `flushRegion`; clip rect; LUT convert in game code |
| Partial-rect push | **PicOS** | `flushRegion`/`flushRows` — no bypass needed |
| Non-blocking present | **PicOS** | DMA double-buffered flush; never waits |
| Input | **PicOS** | `getButtons*`; 20 Hz cap is a platform fact (see §0.6) |
| Timing | **PicOS** | `getTimeMs/Us`, `perf->beginFrame/endFrame` |
| Filesystem | **PicOS** | `fs->*` for tuning/track/asset files; `zip` handles for the asset pack |
| Audio output | **PicOS** | `startStream/pushSamples`; synth in `setAudioCallback` on core1 |
| Frame pacing | **PicOS** | `perf->setTargetFPS` + non-blocking flush |
| Watchdog/system menu | **PicOS** | `sys->poll()` each frame |
| Asset packaging | **PicOS** | zip read-in-place (v5) — one `assets.zip` on SD |
| DMA | Pico SDK (inside PicOS) | already used by display/audio drivers; nothing game-side |
| Multicore | Pico SDK (inside PicOS) | core1 reserved; `setAudioCallback` is the sanctioned hook |
| Interpolator (SIO) | **game** (tiny) | optional: tilemap address gen in blitter inner loop — justify only if profile says the blitter is hot |
| 8bpp indexed scene buffer | **game** | one PSRAM buffer + 256-entry CLUT → back buffer (doom pattern) |
| Mode-7 alternative | PicOS (`drawPlane`) | exists; M1 spike decides ortho-vs-mode7 |
| Fixed-point trig LUTs | **game** | 1024-entry sin/cos + fast atan2 in `core/` |
| Spline/track/pacenotes | **game** (host-baked) | `tools/trackbake` offline |
| Synth audio | **game** | square/saw/LFSR into PCM ring |
| Unit/CI testing | **game** | `plat/headless` stub of the app-facing API |

Nothing in the third column touches hardware behind PicOS's back. No PicOS change
is required for M0–M1; candidate future PicOS improvements (to propose, not hack):
input poll-rate adaptivity, `flushRegion_nocopy` exposure, crypto in sim.

## 2. Open questions carried to M1

1. ~~Mode 7 vs orthographic~~ **DECIDED 2026-07-27: Option A (ortho)** — see §3.
2. ~~Does the STM32 scanner pass 3-key chords?~~ **ANSWERED: yes for
   arrows+SHIFT; 4th key ghosts. Controls must fit in 3 simultaneous.**
3. ~~Is 20 Hz input acceptable?~~ Deferred to M2 feel test with the user driving;
   the 180 ms steer ramp + assist is designed around it. If it feels bad, the fix
   is a PicOS-level poll-rate change, proposed then with data.
   **M1 user drive note: "turning just doesn't seem very responsive"** — first
   tuning target for M2 (ramp times, maxSteer curve, maybe poll-rate proposal).
4. 300 MHz for Rally? Measured cost: full-frame cap drops 57→43 fps. Decide after
   M2 CPU benchmarks — default is 200 MHz. (M1 says NOT needed: sim is 0.15 ms.)
5. Suspend/resume: does PicOS background apps? (Believed no — launcher is modal.
   Confirm; if so, ask user whether in scope.)

## 3. M1 — grey box + Mode 7 spike (2026-07-27, all hardware-measured)

**App**: `apps/rally/` — core/ (mathx LUT trig, tuning parser, bicycle sim,
camera, ortho blitter + 6×8 text on raw BE fb), app/ (PicOS glue, fixed 60 Hz
sim, per-poll edge accumulation, F-key + char-key toggles, autopilot),
plat/headless/ (host build, scripted drive + state hash). 42.5 KB text.

**Measured @200 MHz** (bench logs every 300 frames):
| Metric | Value | Budget | Verdict |
|---|---|---|---|
| sim step (bicycle+assist) | **~0.15 ms** (0.35 ms/frame @2 steps) | ≤2 ms/frame | 13× headroom — float32 fine, no fixed-point swap needed |
| ortho render (320×320 full) | **11.7 ms** | ≤12 ms | at budget; drops to ~8.8 ms at 240-row viewport |
| mode7 render (drawPlane) | 9.4 ms solid / 10.9–13.1 ms noise at speed | — | compute-bound ~20 cycles/px |
| mode7 tex 64² cache-fit vs 256² | 8.9 vs 9.35 ms (~5%) | — | **texture residency a non-issue; PSRAM fine** |
| present (flush call) | ~50 µs | — | non-blocking DMA confirmed; previous frame always done |
| FPS ceiling derived | 57 full / 76 viewport | ≥30 | 30 fps comfortable, 60 plausible |

**§4a decision: ORTHO (Option A), user-approved 2026-07-27.** Mode 7's streaking
(nearest-sample texel stretch) hits exactly the road-read zone at speed at every
parameter combo tried (cam_z 24–64, scale 4–8, horizon 40–160). Fixes don't fit:
bilinear = 2–4× cost, mipmaps = memory+complexity. Ortho pixels land 1:1, art
does depth (PixelLab "high top-down" bakes sides/shadows). drawPlane stays in
firmware unused by us; the mode7 harness code stays behind F1 for reference.

**drawPlane texture format (verified via solid 0x07E0 probe): HOST-order
RGB565** — same swap as every other firmware primitive. Undocumented in os.h;
candidate one-line doc fix upstream (separate commit, later).

**M1 bugs found & fixed (the interesting kind):**
- **shouldExit() is consume-once — TWO call sites = the app can never exit.**
  My pace-loop check swallowed the flag ~99% of frames. One call site only.
- **idle_dim eats the first injected key after the screen dims** (wake-swallow
  by design, clears injection state). Sim has no idle_dim → sim-only divergence.
  Remote-driving workflow: send a neutral wake key first, or use chars.
- **Char injection (`keypress x`) is far more reliable on hardware than button
  injection** (different path: `s_injected_char` vs pending→publish which races
  the wake-swallow). The app mirrors every F-key toggle on char keys
  (m=projection, p=autopilot, d=debug, g=surface, t=tex, -/= cam_z, [/] scale,
  h=horizon, 0-2 pace) — this is the primary remote-control channel.
- **"exit" sent at the LAUNCHER appears to reboot the device mid-command**
  (USB drops). Only send exit when status says an app is actually running.
- Device spontaneously entered USB-MSC mode twice (host sees /dev/sda); serial
  dies until ejected. `udisksctl power-off` clears it but the device then needs
  a physical re-plug. Stale picos_mcp.py processes from old sessions hold the
  port — check `fuser /dev/ttyACM*` before serial work.
- Sim vs hardware: sim drawPlane tramp reads texture from emulated memory and
  renders host-side (fast, correct colors); hardware render cost is the real
  one. Sim input has no idle_dim swallow and no 20 Hz cap.
- `tools/rally_hw.py`-style direct-serial driver (in /tmp, to be promoted into
  tools/ at M2) bypasses MCP port coordination: zip→putb64→unzip push,
  launch/keys/screenshot64. Screenshot64 ≈ 25 s at 115200.

**Feel so far (user, hardware): "drives okay, turning just doesn't seem very
responsive."** → M2: tune ramp times and maxSteer curve first; if the 20 Hz
poll cap is the binding constraint, propose a PicOS poll-rate change with data.

