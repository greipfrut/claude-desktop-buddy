# Claude Desktop Buddy 4B: Audio Beeps + IMU Gestures

**Date:** 2026-06-04
**Branch:** `waveshare-s3-touch-lcd-4b`
**Repo:** https://github.com/greipfrut/claude-desktop-buddy

## Summary

Two post-MVP enhancements that light up hardware the 4B has but the initial
port stubbed out:

1. **Audio beeps** via the ES8311 DAC + speaker — restore the sound feedback
   the original M5StickC build had (approval/deny/navigation/notification
   chirps), gated by the existing `settings().sound` toggle.
2. **IMU gestures** via the QMI8658 6-axis sensor — re-enable shake→dizzy and
   face-down→nap, ported from the original M5 (MPU6886) implementation.

Both are built as small, self-contained driver modules (`audio.*`, `imu.*`)
with narrow public APIs that `main.cpp` drives — the same structural pattern as
the existing `display.*` and `touch.*` modules. The rendering pipeline, BLE
stack, and data/stats layers are untouched.

## Background / current state

- `beep(uint16_t, uint16_t)` exists in `main.cpp:138` as a no-op stub and is
  **never called** — the port stripped all original call sites.
- `settings().sound` (persisted as `s_snd`, default `true`) still exists and is
  wired into the settings menu (`main.cpp:192`), but currently controls nothing.
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
- `beep(freq, dur)` checks `settings().sound`; if enabled, performs a
  **non-blocking** `xQueueSend` with zero timeout and returns. If the queue is
  full the beep is dropped (back-pressure) — `beep()` never blocks the render
  loop. This is the design decision that prevents the ~4-dropped-frame stall a
  blocking 60 ms I2S write would cause at 60 fps.
- The task, per job, synthesizes a **square wave**: half-period in samples =
  `sampleRate / (2 * freq)`, amplitude a fixed moderate level, total samples =
  `sampleRate * dur / 1000`. It writes the buffer to I2S (this blocks the *task*,
  not the loop), then writes a short silence tail so the amp settles, and loops
  back to wait for the next job.
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
`settings().sound`):

| Event | Tone (freq, dur) | Location |
|---|---|---|
| Prompt arrival | 1200 Hz, 80 ms | prompt-change block (`main.cpp:967`) |
| Approval grant | 2400 Hz, 60 ms | touch dispatch, `approve` branch (`main.cpp:1022`) |
| Approval deny | 600 Hz, 60 ms | touch dispatch, deny branch (`main.cpp:1027`) |
| Menu/settings/reset nav tap | 1800 Hz, 30 ms | tap branches (`main.cpp:1030-1047`) |
| Long-press menu toggle | 800 Hz, 60 ms | `G_LONG` branch (`main.cpp:999`) |
| BLE passkey shown | 1800 Hz, 60 ms | passkey-display block |
| Shake → dizzy | 1200 Hz, 60 ms | shake gesture (Feature 2) |

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
with touch reads):

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

**No new settings toggle:** gestures are always on, faithful to the original. A
toggle can be added later if face-down nap proves annoying in practice.

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
| `src/main.cpp` | Remove `beep()` stub; `#include "audio.h"`/`"imu.h"`; call `audioInit()`/`imuInit()` in `setup()`; re-add ~12 `beep()` call sites; add nap/dizzy state machine + `napping` render guard |
| `platformio.ini` | Confirm `ESP_I2S` available (Arduino-ESP32 core); add vendored `src/*.c` to build if needed; no new lib deps (SensorLib already present) |

### Unchanged

`ble_bridge.*`, `data.h`, `stats.h` (already has the `sound` setting), `xfer.h`,
`clock.h`, `touch.*`, `character.*`, `buddy.*`, all `buddies/*.cpp`, GIF assets.

## Implementation order

### Phase 1: Audio (compile-first, then on-device)

1. Vendor `es8311.c/.h/_reg.h` into `src/`; fix include paths if needed
   (e.g. `freertos/FreeRTOS.h`).
2. Write `audio.h` / `audio.cpp` (init, synth, task, queue).
3. Expose `expander` from `display.h`.
4. `pio run` — compile clean.
5. Flash; verify a single test beep at boot sounds through the speaker.
6. Re-add all `beep()` call sites in `main.cpp`; verify each event sounds and
   that toggling `settings().sound` off silences them.

### Phase 2: IMU gestures (compile-first, then on-device tuning)

1. Write `imu.h` / `imu.cpp`.
2. Add init call + gesture state machine + nap render guard in `main.cpp`.
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
