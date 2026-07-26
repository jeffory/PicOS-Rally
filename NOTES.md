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

### 0.9 Hardware measurements (PENDING — device session)

| Measurement | Prediction from code | Measured | Verdict |
|---|---|---|---|
| Full-frame push 320×320 | 16.4 ms @100 MHz | | |
| Viewport push 320×240 | 12.3 ms | | |
| Same @300 MHz | 21.8 / 16.4 ms | | |
| Key press→`getButtons` latency | ≤ ~60 ms | | |
| 3-key rollover (up+left+space etc.) | protocol supports; scanner TBD | | |
| Poll interval effective rate | 20 Hz | | |
| `drawPlane` 240-line cost | ~5 ms class | (M1 spike) | |
| Sim step (2× bicycle) | ≤2 ms budget | (M1) | |

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

1. Mode 7 vs orthographic — M1 spike with measured numbers, decide together.
2. Does the STM32 scanner pass 3-key chords? (§0.7 test) — affects control design.
3. Is 20 Hz input + 50 ms worst-case latency acceptable feel at 180 ms steer ramp?
   Measure, then decide if a PicOS input-path improvement is worth proposing.
4. 300 MHz for Rally? (+50 % CPU, −25 % display rate; doom uses it.) Decide after
   measuring sim+render cost at 200 MHz.
5. Suspend/resume: does PicOS background apps? (Believed no — launcher is modal.
   Confirm; if so, ask user whether in scope.)
