# Claude Desktop Buddy 4B: Audio Beeps + IMU Gestures

**Date:** 2026-06-04
**Branch:** `waveshare-s3-touch-lcd-4b`
**Repo:** https://github.com/greipfrut/claude-desktop-buddy

## Summary

Two post-MVP enhancements that light up hardware the 4B has but the initial
port stubbed out:

1. **Audio beeps** via the ES8311 DAC + speaker — restore the sound feedback
   the original M5StickC build had (approval/deny/navigation/notification
   chirps), with a new **adjustable volume** (off + 4 levels) and a
   **square/sine waveform toggle** so the user can pick the tone character by
   ear.
2. **IMU gestures** via the QMI8658 6-axis sensor — re-enable shake→dizzy and
   face-down→nap, ported from the original M5 (MPU6886) implementation, behind
   a new **gesture on/off toggle**.

Three new user settings (volume, waveform, gestures) are added to the settings
menu. Because the menu is a fixed-height list on the 240×320 canvas with a
practical ceiling of ~10 rows, volume and waveform live in a new **Audio
submenu** (reached from the existing `sound` row, reusing the `reset`-submenu
pattern), and `gesture` becomes one new top-level toggle row.

Both are built as small, self-contained driver modules (`audio.*`, `imu.*`)
with narrow public APIs that `main.cpp` drives — the same structural pattern as
the existing `display.*` and `touch.*` modules. The rendering pipeline, BLE
stack, and data/stats layers are untouched.

## Background / current state

- `beep(uint16_t, uint16_t)` exists in `main.cpp:138` as a no-op stub and is
  **never called** — the port stripped all original call sites.
- `settings().sound` (persisted as `s_snd`, default `true`) still exists and is
  wired into the settings menu (`main.cpp:192`), but currently controls nothing.
  It is **replaced** by `settings().volume` (level 0-4; 0 = off) — `beep()` is
  gated on `volume > 0`, so the old binary mute folds into level 0.
- `drawSettings()` (`main.cpp:264`) renders the value column with brittle
  hardcoded index arithmetic (`vi = (i <= 2) ? i - 1 : i - 2`). Inserting the
  new `gesture` row shifts every index, so that value-column logic is reworked
  into a per-item `switch` keyed off the item rather than fragile offsets — a
  targeted cleanup scoped to this change.
- The original M5 build called `beep()` at ~12 sites and had two IMU gestures
  (`checkShake()`, `isFaceDown()`); all of that was removed during the port.
- `SensorQMI8658` ships inside `lewisxhe/SensorLib`, which is **already a
  dependency** (used for the GT911 touch driver). No new library is required.
- The 4B shares one I2C bus (`Wire`, SDA 47 / SCL 48) across the GT911 touch,
  XCA9554 IO expander, QMI8658 IMU, and ES8311 codec. `Wire.begin(47, 48)` is
  already called in `displayInit()`.

## Hardware reference

| Peripheral | Bus / pins | Notes |
|---|---|---|
| ES8311 DAC | I2C `Wire` @ 0x18 (`ES8311_ADDRRES_0`); I2S BCLK=16, LRCK=7, DOUT=6, DIN=15, MCLK=5 | Speaker amp enable = XCA9554 expander pin 3 (drive HIGH) |
| QMI8658 IMU | I2C `Wire` @ `QMI8658_L_SLAVE_ADDRESS` | Accelerometer only; gyro unused |

DIN=15 is the ES7210 mic input — not needed for output-only beeps, but passed
to `i2s.setPins()` to match the demo's STD-mode configuration.

## Architecture

### New modules

```
src/audio.h / src/audio.cpp   — ES8311 + I2S ownership, beep() API, audio task
src/imu.h   / src/imu.cpp     — QMI8658 ownership, gesture predicates
src/es8311.c / .h / _reg.h    — vendored codec driver (copied from 4B demo 07)
```

The **nap/dizzy state machine lives in `main.cpp`**, not in the imu module — it
mutates persona state, accumulates stats, and controls the screen, so it belongs
with the other loop logic. `imu.*` only owns the sensor and exposes raw reads +
two stateless predicates. This is the same division the original M5 code used,
with the driver factored out for isolation/testability.

### Feature 1: Audio (`audio.h` / `audio.cpp`)

**Public API** — drop-in replacement for the current stub, so re-added call
sites need no special signature:

```cpp
void audioInit();                       // I2S pins, codec init, amp enable, spawn task
void beep(uint16_t freq, uint16_t dur); // enqueue a tone; returns immediately
```

**Async playback architecture:**

- A FreeRTOS task (`audio_task`), pinned to core 1, owns the I2S peripheral and
  blocks on a small FreeRTOS queue (`QueueHandle_t`, depth ~4) of `{freq, dur}`
  jobs.
- `beep(freq, dur)` checks `settings().volume > 0`; if audible, performs a
  **non-blocking** `xQueueSend` with zero timeout and returns. If the queue is
  full the beep is dropped (back-pressure) — `beep()` never blocks the render
  loop. This is the design decision that prevents the ~4-dropped-frame stall a
  blocking 60 ms I2S write would cause at 60 fps.
- The task, per job, snapshots `settings().volume` and `settings().sineWave` and
  synthesizes the tone over `sampleRate * dur / 1000` samples:
  - **Square wave** (`sineWave == false`): output alternates `±amp` every
    half-period (`sampleRate / (2 * freq)` samples). Cheap, punchy, closest to
    the original piezo character.
  - **Sine wave** (`sineWave == true`): `amp * sinf(2π * freq * n / sampleRate)`
    per sample. Softer tone.
  - `amp` scales with volume level so loudness is controlled purely in the
    synthesized PCM (no per-beep I2C to the codec). The ES8311 voice volume is
    set once at init to a fixed high value; the four volume levels map to
    amplitude fractions of full-scale int16 (e.g. ~0.15 / 0.35 / 0.6 / 1.0).
  - It writes the buffer to I2S (blocking the *task*, not the loop), then writes
    a short silence tail so the amp settles, and loops back for the next job.
- Sample rate 16 kHz (matches demo). Nyquist (8 kHz) comfortably covers the
  highest beep (2400 Hz).

**Init sequence** — `audioInit()` called from `setup()` after `displayInit()`
(Wire and the expander are already up):

1. Enable speaker amp: `expander->digitalWrite(3, HIGH)`. This requires exposing
   the existing expander instance from `display.h` via
   `extern Arduino_XCA9554SWSPI *expander;` rather than constructing a second
   XCA9554 handle on the same chip.
2. `i2s.setPins(16, 7, 6, 15, 5)` then
   `i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)`.
3. `es8311_codec_init()` — 16 kHz, MCLK = 256× sample rate, mic disabled, fixed
   voice volume (~80/100).
4. `xTaskCreatePinnedToCore(audio_task, ...)`.

If any step fails, log to serial and leave `beep()` a safe no-op (audio absent
must never crash the buddy).

**Re-added call sites** (faithful to the original; all gated inside `beep()` by
`settings().volume > 0`):

| Event | Tone (freq, dur) | Location |
|---|---|---|
| Prompt arrival | 1200 Hz, 80 ms | prompt-change block (`main.cpp:967`) |
| Approval grant | 2400 Hz, 60 ms | touch dispatch, `approve` branch (`main.cpp:1022`) |
| Approval deny | 600 Hz, 60 ms | touch dispatch, deny branch (`main.cpp:1027`) |
| Menu/settings/reset nav tap | 1800 Hz, 30 ms | tap branches (`main.cpp:1030-1047`) |
| Long-press menu toggle | 800 Hz, 60 ms | `G_LONG` branch (`main.cpp:999`) |
| BLE passkey shown | 1800 Hz, 60 ms | passkey-display block |
| Shake → dizzy | 1200 Hz, 60 ms | shake gesture (Feature 2) |

### Settings & UI

**New `Settings` fields** (`stats.h`), replacing `bool sound`:

| Field | Type | Default | NVS key | Meaning |
|---|---|---|---|---|
| `volume` | `uint8_t` | 3 | `s_vol` | 0 = off, 1-4 = louder |
| `sineWave` | `bool` | `false` | `s_wav` | false = square, true = sine |
| `gestures` | `bool` | `true` | `s_gst` | shake + face-down enabled |

`statsLoad`/`settingsSave` gain the three keys; the obsolete `s_snd` key is
dropped.

**Top-level menu** — `settingsItems[]` becomes (10 rows, fits the 320px height):

```
{ "bright", "sound", "gesture", "bt", "pair", "led", "hud", "pet", "reset", "back" }
```

- `sound` row no longer toggles; tapping it opens the Audio submenu
  (`audioOpen = true`), exactly as `reset` opens the reset submenu. Its value
  column shows the current volume (`"off"` or `"n/4"`).
- `gesture` row is a plain on/off toggle of `settings().gestures`, rendered like
  `led`/`hud`.
- `drawSettings()`'s value column is reworked from index arithmetic to a
  `switch` keyed on the item so adding/removing rows no longer requires
  recomputing offsets.

**Audio submenu** — mirrors the existing `reset` submenu machinery
(`audioOpen`/`audioSel`, `audioItems[]`, `applyAudio()`, `drawAudio()`,
`hitAudio()`):

```
audioItems[] = { "volume", "wave", "back" }
```

- `volume`: cycles `0→1→2→3→4→0` (like `bright`); value column shows `off` /
  `n/4`.
- `wave`: toggles square ↔ sine; value column shows `sqr` / `sin`.
- After changing `volume` (to a non-zero level) or `wave`, immediately call
  `beep(1800, 80)` so the user hears the new setting — this is the by-ear
  comparison the waveform toggle is for. Setting volume to 0 stays silent.
- `back`: closes the submenu, returns to the top-level settings list.

`settingsSave()` is called after each change, as with the other settings.

### Feature 2: IMU gestures (`imu.h` / `imu.cpp`)

**Public API:**

```cpp
bool imuInit();                                 // qmi.begin + configAccelerometer + enable
bool imuRead(float* ax, float* ay, float* az);  // latest accel sample in g; false if not ready
bool checkShake();                              // running-avg magnitude delta exceeds threshold
bool isFaceDown();                              // z-axis dominant and negative
```

- `imuInit()` runs in `setup()` after `displayInit()`. Calls
  `qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, 47, 48)`,
  `qmi.configAccelerometer(ACC_RANGE_4G, <modest ODR>, LPF_MODE_0)`,
  `qmi.enableAccelerometer()`. Gyro stays off — neither gesture needs it.
  Returns false on failure; gestures then degrade to no-ops.
- `checkShake()` ports the original: maintains a running-average accel magnitude
  baseline (`baseline = baseline*0.95 + mag*0.05`) and returns true when the
  instantaneous delta exceeds a threshold (original used 0.8 g).
- `isFaceDown()` ports the original: `az < -0.7 && |ax| < 0.4 && |ay| < 0.4`.
  Frame debouncing stays in `main.cpp`.

**Gesture state machine in `main.cpp` loop** (polled every ~50 ms via a
`lastImuCheck` gate, matching the original cadence and avoiding I2C contention
with touch reads). The whole block is skipped when `settings().gestures` is
off (and any active nap is released so the buddy doesn't get stuck asleep):

- **Shake → dizzy:** when `checkShake()` and not (`menuOpen || screenOff`) and no
  one-shot active → `triggerOneShot(P_DIZZY, 2000)` and `beep(1200, 60)`.
- **Face-down → nap:** a `faceDownFrames` counter (enter threshold +15, exit −8,
  clamped) debounces noise.
  - On entry (`!napping && faceDownFrames >= 15`): set `napping = true`, record
    `napStartMs`, pause animation.
  - On exit (`napping && faceDownFrames <= -8`): `napping = false`,
    `statsOnNapEnd((now - napStartMs) / 1000)`, restore the display.

**Nap visuals (decision):** because 4B backlight brightness is stubbed
(always-on until AXP2101 work lands), nap **fills the canvas black and blits
once**, then skips sprite rendering while `napping` — extending the existing
`napping || screenOff` guard in the render block. The backlight stays physically
lit, but a black screen is the closest available approximation of "asleep." Nap
stats accumulate exactly as in the original.

**Calibration caveat:** the QMI8658's physical mounting orientation on the 4B
almost certainly differs from the M5StickC's MPU6886, so axis signs and the
`az < -0.7` face-down threshold (and possibly the shake threshold) will need
empirical tuning on real hardware. This is an explicit on-device step in the
implementation order, supported by temporary serial logging of raw ax/ay/az.

**Gesture toggle:** the new `settings().gestures` switch (default on) lets the
user disable shake + face-down entirely — useful if face-down nap proves
intrusive. See the Settings & UI section.

## File change map

### New files

| File | Purpose |
|---|---|
| `src/audio.h` | `audioInit()`, `beep()` declarations |
| `src/audio.cpp` | I2S + ES8311 init, square-wave synth, audio task, queue |
| `src/imu.h` | `imuInit()`, `imuRead()`, `checkShake()`, `isFaceDown()` declarations |
| `src/imu.cpp` | QMI8658 init + read + gesture predicates |
| `src/es8311.c` | Vendored codec driver (from demo `07_ES8311`) |
| `src/es8311.h` | Vendored codec driver header |
| `src/es8311_reg.h` | Vendored codec register map |

### Modified files

| File | Changes |
|---|---|
| `src/display.h` | Add `extern Arduino_XCA9554SWSPI *expander;` so audio can drive amp-enable pin 3 |
| `src/display.cpp` | Ensure `expander` has external linkage (drop `static` if present) |
| `src/stats.h` | Replace `bool sound` with `uint8_t volume`, `bool sineWave`, `bool gestures`; update load/save (drop `s_snd`, add `s_vol`/`s_wav`/`s_gst`) |
| `src/main.cpp` | Remove `beep()` stub; `#include "audio.h"`/`"imu.h"`; call `audioInit()`/`imuInit()` in `setup()`; re-add ~12 `beep()` call sites; add nap/dizzy state machine + `napping` render guard; add `gesture` top-level row; add Audio submenu (`audioOpen`/`applyAudio`/`drawAudio`/`hitAudio`); rework `drawSettings()` value column off index arithmetic |
| `platformio.ini` | Confirm `ESP_I2S` available (Arduino-ESP32 core); add vendored `src/*.c` to build if needed; no new lib deps (SensorLib already present) |

### Unchanged

`ble_bridge.*`, `data.h`, `xfer.h`, `clock.h`, `touch.*`, `character.*`,
`buddy.*`, all `buddies/*.cpp`, GIF assets.

## Implementation order

### Phase 1: Audio (compile-first, then on-device)

1. Vendor `es8311.c/.h/_reg.h` into `src/`; fix include paths if needed
   (e.g. `freertos/FreeRTOS.h`).
2. Write `audio.h` / `audio.cpp` (init, square+sine synth, volume scaling, task,
   queue).
3. Expose `expander` from `display.h`.
4. `pio run` — compile clean.
5. Flash; verify a single test beep at boot sounds through the speaker.
6. Re-add all `beep()` call sites in `main.cpp`; verify each event sounds.

### Phase 2: Settings & UI

1. Update `stats.h`: replace `sound` with `volume`/`sineWave`/`gestures` and
   their NVS keys.
2. Add the `gesture` top-level row and rework `drawSettings()`'s value column.
3. Add the Audio submenu (`audioOpen`/`audioSel`/`audioItems`/`applyAudio`/
   `drawAudio`/`hitAudio`), with the sample beep on volume/wave change.
4. `pio run` — compile clean; flash. Verify: volume off silences all beeps;
   each level is audibly louder; the wave toggle changes tone character; the
   sample beep plays on change.

### Phase 3: IMU gestures (compile-first, then on-device tuning)

1. Write `imu.h` / `imu.cpp`.
2. Add init call + gesture state machine (gated on `settings().gestures`) +
   nap render guard in `main.cpp`.
3. `pio run` — compile clean.
4. Flash; enable temporary serial logging of raw ax/ay/az.
5. Tune face-down threshold and shake threshold against the 4B's actual mount
   orientation; confirm shake→dizzy and face-down→nap behave; remove temp logs.

## Risks / open items

- **Vendored driver build:** `es8311.c` pulls in ESP-IDF headers
  (`esp_log.h`, `esp_check.h`, `esp32-hal-i2c.h`, FreeRTOS). All ship with the
  Arduino-ESP32 core, but include paths or `FreeRTOS.h` vs
  `freertos/FreeRTOS.h` may need a one-line fix at build time.
- **I2C contention:** ES8311 is configured once over I2C then streams over I2S
  (no I2C during playback). QMI8658 polled at 20 Hz. Combined load on the shared
  bus with the GT911 touch should be well within budget.
- **Audio peripheral conflict:** the RGB panel uses its own peripheral; I2S is
  independent. No expected conflict, to be confirmed on-device.
- **IMU thresholds are placeholders** until on-device tuning (Phase 2).

## Reference paths

- Buddy repo: `C:\Users\tsphan-ahc\Development\claude-desktop-buddy`
- 4B demo code: `C:\Users\tsphan-ahc\Downloads\ESP32-S3-Touch-LCD-4B-Demos`
- ES8311 demo: `...\Arduino-v3.2.0\examples\07_ES8311\`
- QMI8658 demo: `...\Arduino-v3.2.0\examples\04_LVGL_QMI8658_ui\`
- Pin config: `...\Arduino-v3.2.0\libraries\Mylibrary\pin_config.h`
- Original M5 gesture/beep source: git rev `8ac960d:src/main.cpp`
- PlatformIO: `C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe` (COM4)

## IMPLEMENTATION ADDENDUM (discovered during build-out)

### Reboot crash — root cause + fix

The 4B spontaneously reboots under BLE workload: a panic `Cache disabled but
cached memory region accessed` (EXCCAUSE 0x7) in the RGB-LCD restart ISR, which
(with `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y`) calls GDMA control functions that the
precompiled libs leave in flash (`CONFIG_GDMA_CTRL_FUNC_IN_IRAM` unset). Any
flash op that disables the cache makes the per-VSYNC ISR fault.

- **Band-aid shipped (commit 307fa4a):** the dominant trigger was the BLE "status"
  reply computing `fsFree` via `LittleFS.usedBytes()`/`totalBytes()` — each walks
  the whole filesystem. Both are cached now (`xferRefreshFsUsed()`), refreshed only
  at boot (before the panel starts) and after a transfer. Uptime <2 min → 13+ min.
- **Complete fix:** `CONFIG_GDMA_CTRL_FUNC_IN_IRAM=y` (needs the rebuild below).

### Audio — root cause + why a rebuild is required

`audioInit()`'s `i2s.begin()` FAILS on the precompiled libs:
`gdma: gdma_register_tx_event_callbacks: user context not in internal RAM`.
Cause: `CONFIG_GDMA_ISR_IRAM_SAFE=y` (GDMA requires an internal-RAM DMA context)
but `CONFIG_I2S_ISR_IRAM_SAFE` is unset (I2S context can land in PSRAM). So
`audioReady` stays false and every `beep()` is a no-op (silent). NOT a memory
shortage (261 KB internal free, still fails). The manufacturer demos only work
because they were built against a different toolchain config.

Audio tasks 1–4 are committed and correct (settings UI, Audio submenu, ES8311
driver, beep call sites) but produce no sound until the framework is rebuilt.

### Proven-good flags (from the 4B Brookesia ESP-IDF demo, which plays audio
AND drives the RGB panel on this exact board —
`...\ESP-IDF-v5.4.2\03_esp-brookesia\sdkconfig`):
`CONFIG_GDMA_ISR_IRAM_SAFE` **off**, `CONFIG_GDMA_CTRL_FUNC_IN_IRAM=y`,
`CONFIG_LCD_RGB_ISR_IRAM_SAFE` off, `CONFIG_LCD_RGB_RESTART_IN_VSYNC` off.

### Next-session rebuild recipe (fixes audio + crash together)

1. **platformio.ini `custom_sdkconfig`:**
   - `CONFIG_GDMA_ISR_IRAM_SAFE=n`   ← makes `i2s.begin()` succeed (audio)
   - `CONFIG_GDMA_CTRL_FUNC_IN_IRAM=y`   ← complete RGB-panel crash fix
   - `CONFIG_ARDUINO_SELECTIVE_COMPILATION=y` + a `CONFIG_ARDUINO_SELECTIVE_<lib>=y`
     line for every Arduino lib actually used (≥ `BLE`, `FS`, `LittleFS`,
     `Preferences`, `Wire`, `SPI`; add more if the link fails on a missing symbol).
     Do NOT enable `Insights`, `RainMaker`, `ESP_SR`, `Zigbee`, `Matter`,
     `OpenThread` — leaving them out is what stops `esp_insights` from building and
     dodges the cert-embed bug (pioarduino never generates `https_server.crt.S`;
     switching `ESP_INSIGHTS_TRANSPORT_MQTT` does NOT help).
2. **Reimplement `audio.cpp` on the IDF I2S driver** (`driver/i2s_std.h`):
   SELECTIVE_COMPILATION drops the `ESP_I2S` Arduino wrapper (it has no selective
   flag). Keep the synth/`AMP[]`/queue/`audioTask`/`beep()` logic verbatim; replace
   `I2SClass i2s` + `i2s.setPins/begin/write` with `i2s_chan_handle_t` +
   `i2s_new_channel` / `i2s_channel_init_std_mode` / `i2s_channel_enable` /
   `i2s_channel_write`. ES8311 codec init (es8311.c) is unchanged.
3. **Wipe `.pio/build/<env>` before building** (precompiled↔source switch leaves a
   stale CMake cache → bogus doubled generated-source paths). First source build is
   slow (downloads + compiles all of IDF; exceeds the 10-min background cap — just
   re-launch and it resumes incrementally from cache).
4. **Verify:** temporarily set `-DARDUINO_USB_CDC_ON_BOOT=0` so app `Serial` reaches
   COM4 (this board's COM4 is a CH343→UART0; app Serial otherwise goes to the
   native-USB pins, invisible). Look for `[audio] ready` + a boot self-test beep,
   then revert to `=1`.

### IMU gestures (Tasks 5–7) need no rebuild

QMI8658 is I2C polling via SensorLib (no DMA), unaffected by the GDMA/I2S config.
Tasks 5–7 can be implemented and run on the current precompiled libs.
