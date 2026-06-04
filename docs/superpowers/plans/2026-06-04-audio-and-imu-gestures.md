# Audio Beeps + IMU Gestures Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add ES8311 audio beep feedback (with adjustable volume + square/sine waveform) and QMI8658 IMU gestures (shake→dizzy, face-down→nap, toggleable) to the Waveshare 4B buddy firmware.

**Architecture:** Two new self-contained driver modules (`audio.*`, `imu.*`) with narrow public APIs that `main.cpp` drives, mirroring the existing `display.*`/`touch.*` pattern. Audio plays asynchronously via a FreeRTOS task + queue so `beep()` never blocks the render loop. The nap/dizzy state machine lives in `main.cpp` and consumes stateless IMU predicates. Three new settings (volume, waveform, gestures) extend the settings menu; volume + waveform sit in an Audio submenu reusing the existing reset-submenu pattern.

**Tech Stack:** PlatformIO + Arduino-ESP32 (pioarduino), Arduino_GFX, `ESP_I2S` (Arduino core), vendored ES8311 C driver, `lewisxhe/SensorLib` (QMI8658, already a dependency), FreeRTOS.

**Verification model:** This is embedded firmware with no unit-test framework. Each task is verified by (a) `pio run` compiling clean and (b) explicit on-device observation. The build/flash/monitor commands (PowerShell, board on COM4):

```powershell
& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b
& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b -t upload --upload-port COM4
```

**Note on the reset-reason diagnostic:** `main.cpp` currently carries a temporary `resetReasonStr()` probe + `[diag]` print in `setup()` (added during an unrelated reboot investigation). **Leave it in place** — it is not part of this feature and will be removed when the reboot issue is resolved.

---

## File Structure

### New files

| File | Responsibility |
|---|---|
| `src/audio.h` | Public audio API: `audioInit()`, `beep(freq, dur)` |
| `src/audio.cpp` | ES8311 + I2S init, tone synthesis (square/sine, volume-scaled), FreeRTOS audio task + queue |
| `src/imu.h` | Public IMU API: `imuInit()`, `imuRead()`, `checkShake()`, `isFaceDown()` |
| `src/imu.cpp` | QMI8658 init + accel read + gesture predicates |
| `src/es8311.c` | Vendored ES8311 codec driver (from 4B demo `07_ES8311`) |
| `src/es8311.h` | Vendored codec driver header (already `extern "C"`-guarded) |
| `src/es8311_reg.h` | Vendored codec register map |

### Modified files

| File | Changes |
|---|---|
| `src/stats.h` | Replace `bool sound` with `uint8_t volume` + `bool sineWave` + `bool gestures`; update default/load/save |
| `src/main.cpp` | Settings menu rework + Audio submenu; replace `beep()` stub with `audio.h`; `audioInit()`/`imuInit()` in `setup()`; re-add beep call sites; gesture state machine + nap render guard |

`expander` is already declared `extern` in `display.h:18` and defined non-`static` in `display.cpp:9`, so `audio.cpp` can drive the amp-enable pin without touching `display.*`.

---

## Task 1: Settings model + menu rework

Replaces the binary `sound` setting with `volume`/`sineWave`/`gestures`, adds the `gesture` top-level row, and adds the Audio submenu. Done as one atomic change because removing `Settings::sound` breaks `main.cpp` references that must update together. The no-op `beep()` stub still exists at this point, so the sample-beep calls compile (and do nothing until Task 3).

**Files:**
- Modify: `src/stats.h:179-208`
- Modify: `src/main.cpp` (settings globals, `applySetting`, `drawSettings`, hit-testers, touch dispatch, render dispatch)

- [ ] **Step 1: Replace the `Settings` struct, default, load, and save in `src/stats.h`**

Replace lines 179-208 (`struct Settings { ... }` through the end of `settingsSave()`) with:

```cpp
struct Settings {
  uint8_t volume;    // 0 = off, 1..4 = louder
  bool    sineWave;  // false = square, true = sine
  bool    gestures;  // shake + face-down enabled
  bool    bt;
  bool    led;
  bool    hud;
  uint8_t bright;   // 0..4 → 20..100% backlight
};

static Settings _settings = { 3, false, true, true, true, true, 2 };

inline void settingsLoad() {
  _prefs.begin("buddy", true);
  _settings.volume   = _prefs.getUChar("s_vol", 3);
  if (_settings.volume > 4) _settings.volume = 3;
  _settings.sineWave = _prefs.getBool("s_wav", false);
  _settings.gestures = _prefs.getBool("s_gst", true);
  _settings.bt       = _prefs.getBool("s_bt",  true);
  _settings.led      = _prefs.getBool("s_led", true);
  _settings.hud      = _prefs.getBool("s_hud", true);
  _settings.bright   = _prefs.getUChar("s_bri", 2);
  if (_settings.bright > 4) _settings.bright = 2;
  _prefs.end();
}

inline void settingsSave() {
  _prefs.begin("buddy", false);
  _prefs.putUChar("s_vol", _settings.volume);
  _prefs.putBool("s_wav",  _settings.sineWave);
  _prefs.putBool("s_gst",  _settings.gestures);
  _prefs.putBool("s_bt",   _settings.bt);
  _prefs.putBool("s_led",  _settings.led);
  _prefs.putBool("s_hud",  _settings.hud);
  _prefs.putUChar("s_bri", _settings.bright);
  _prefs.end();
}
```

- [ ] **Step 2: Update the settings item list in `src/main.cpp`**

Replace lines 173-174 (`settingsItems[]` + `SETTINGS_N`):

```cpp
const char* settingsItems[] = { "bright", "sound", "gesture", "bt", "pair", "led", "hud", "pet", "reset", "back" };
const uint8_t SETTINGS_N = 10;
```

Immediately after the `resetItems`/`RESET_N` block (around line 178-179), add the Audio submenu state:

```cpp
bool    audioOpen = false;
uint8_t audioSel  = 0;
const char* audioItems[] = { "volume", "wave", "back" };
const uint8_t AUDIO_N = 3;
```

- [ ] **Step 3: Rework `applySetting()` and add `applyAudio()` in `src/main.cpp`**

Replace the whole `applySetting()` function (currently lines ~185-202) with:

```cpp
static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0: s.bright = (s.bright + 1) % 5; applyBrightness(); break;
    case 1: audioOpen = true; audioSel = 0; return;   // sound → Audio submenu
    case 2: s.gestures = !s.gestures; break;
    case 3: s.bt = !s.bt; break;
    case 4: pairTrigger = true; return;
    case 5: s.led = !s.led; break;
    case 6: s.hud = !s.hud; break;
    case 7: nextPet(); return;
    case 8: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 9: settingsOpen = false; redrawAll(); return;
  }
  settingsSave();
}

static void applyAudio(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0: s.volume = (s.volume + 1) % 5; break;   // 0..4, 0 = off
    case 1: s.sineWave = !s.sineWave; break;
    case 2: audioOpen = false; return;              // back to settings list
  }
  settingsSave();
  beep(1800, 80);   // sample the new setting by ear (silent if volume==0)
}
```

- [ ] **Step 4: Rework the `drawSettings()` value column in `src/main.cpp`**

In `drawSettings()` (currently ~264-302), delete the `bool vals[] = { ... };` line and replace the per-row value-column block (the `if (i == 0) ... else if ...` chain, currently ~282-296) with a switch keyed on the row index:

```cpp
    spr.setTextSize(2);
    spr.setCursor(mx + mw - 64, my + 16 + i * 28);
    spr.setTextColor(p.textDim, PANEL);
    switch (i) {
      case 0: spr.printf("%u/4", s.bright); break;                     // bright
      case 1:                                                           // sound → volume
        if (s.volume == 0) spr.print("off");
        else               spr.printf("%u/4", s.volume);
        break;
      case 2: spr.setTextColor(s.gestures ? GREEN : p.textDim, PANEL);  // gesture
              spr.print(s.gestures ? " on" : "off"); break;
      case 3: spr.setTextColor(s.bt ? GREEN : p.textDim, PANEL);        // bt toggle
              spr.print(s.bt ? " on" : "off"); break;
      case 4:                                                           // pair status
        if (!s.bt)                  { spr.print("off"); }
        else if (bleConnected())    { spr.setTextColor(GREEN, PANEL); spr.print(" ok"); }
        else                        { spr.setTextColor(HOT, PANEL);   spr.print(" go"); }
        break;
      case 5: spr.setTextColor(s.led ? GREEN : p.textDim, PANEL);       // led
              spr.print(s.led ? " on" : "off"); break;
      case 6: spr.setTextColor(s.hud ? GREEN : p.textDim, PANEL);       // hud
              spr.print(s.hud ? " on" : "off"); break;
      case 7: {                                                         // pet
        uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
        uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
        spr.printf("%u/%u", pos, total);
        break;
      }
      default: break;   // 8 reset, 9 back: no value column
    }
    spr.setTextSize(3);
```

(`Settings& s = settings();` already exists at the top of `drawSettings()`; keep it.)

- [ ] **Step 5: Add `drawAudio()` in `src/main.cpp`**

Immediately after `drawSettings()`'s closing brace (~line 302), add:

```cpp
static void drawAudio() {
  const Palette& p = characterPalette();
  Settings& s = settings();
  int mw = 236, mh = 16 + AUDIO_N * 28 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 8, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 8, p.textDim);
  spr.setTextSize(3);
  for (int i = 0; i < AUDIO_N; i++) {
    bool sel = (i == audioSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 12 + i * 28);
    spr.print(sel ? "> " : "  ");
    spr.print(audioItems[i]);
    spr.setTextSize(2);
    spr.setCursor(mx + mw - 64, my + 16 + i * 28);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      if (s.volume == 0) spr.print("off");
      else               spr.printf("%u/4", s.volume);
    } else if (i == 1) {
      spr.print(s.sineWave ? "sin" : "sqr");
    }
    spr.setTextSize(3);
  }
  drawMenuHints(p, mx, mw, my + mh - 14);
  spr.setTextSize(1);
}
```

- [ ] **Step 6: Add `hitAudio()` in `src/main.cpp`**

Immediately after `hitReset()` (~line 876), add:

```cpp
static int hitAudio(int ty) {
  int mh = 16 + AUDIO_N * 28 + MENU_HINT_H;
  int my = (H - mh) / 2;
  int firstY = my + 10;
  int i = (ty - firstY) / 28;
  return (i >= 0 && i < AUDIO_N) ? i : -1;
}
```

- [ ] **Step 7: Wire the Audio submenu into the touch dispatch in `src/main.cpp`**

In the `G_LONG` branch (currently ~999-1006), add an `audioOpen` close case between `resetOpen` and `settingsOpen`:

```cpp
  if (g == G_LONG) {
    if (resetOpen) { resetOpen = false; redrawAll(); }
    else if (audioOpen) { audioOpen = false; redrawAll(); }
    else if (settingsOpen) { settingsOpen = false; redrawAll(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) redrawAll();
    }
  } else if (g == G_TAP) {
```

In the `G_TAP` branch, add an `audioOpen` case immediately before the `else if (resetOpen)` case (currently ~1030):

```cpp
    } else if (audioOpen) {
      int r = hitAudio(ty);
      if (r >= 0) { audioSel = r; applyAudio(r); }
    } else if (resetOpen) {
```

- [ ] **Step 8: Render the Audio submenu in `src/main.cpp`**

In the overlay render block (currently ~1149-1152), add `drawAudio()` between `drawReset()` and `drawSettings()`:

```cpp
    if (resetOpen) drawReset();
    else if (audioOpen) drawAudio();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
```

- [ ] **Step 9: Compile**

Run: `& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b`
Expected: `[SUCCESS]`. No references to `settings().sound` / `s.sound` remain.

- [ ] **Step 10: Flash and verify on device**

Flash, open settings (long-press → menu → settings). Verify:
- A `gesture` row shows `on`/`off` and toggles on tap.
- The `sound` row shows the volume (`3/4`) and opens the Audio submenu on tap.
- In the Audio submenu, `volume` cycles `off`→`1/4`…`4/4`, `wave` toggles `sqr`/`sin`, `back` (and hold-to-close) returns to the settings list.
- No audio yet (expected — `beep()` is still the no-op stub).

- [ ] **Step 11: Commit**

```powershell
git add src/stats.h src/main.cpp
git commit -m @'
feat: volume/waveform/gesture settings + Audio submenu

Replace binary sound setting with volume (0-4), sineWave toggle, and
gestures toggle. Add gesture top-level row and an Audio submenu (volume,
wave) reusing the reset-submenu pattern. Rework drawSettings value column
off brittle index arithmetic. beep() is still a stub until the audio module.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 2: Vendor the ES8311 codec driver

**Files:**
- Create: `src/es8311.c`, `src/es8311.h`, `src/es8311_reg.h` (copied from the demo)

- [ ] **Step 1: Copy the three driver files into `src/`**

```powershell
$demo = "C:\Users\tsphan-ahc\Downloads\ESP32-S3-Touch-LCD-4B-Demos\Arduino-v3.2.0\examples\07_ES8311"
Copy-Item "$demo\es8311.c"     "src\es8311.c"
Copy-Item "$demo\es8311.h"     "src\es8311.h"
Copy-Item "$demo\es8311_reg.h" "src\es8311_reg.h"
```

- [ ] **Step 2: Compile to surface any include-path issues**

Run: `& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b`
Expected: `[SUCCESS]`. (The files compile but are not yet referenced — `build_src_filter` already includes `+<*>`, which picks up `.c` files in `src/`.)

If the build fails with `FreeRTOS.h: No such file or directory`, open `src/es8311.c` and change:

```c
#include "FreeRTOS.h"
```
to:
```c
#include "freertos/FreeRTOS.h"
```
Then re-run the build. Expected: `[SUCCESS]`.

- [ ] **Step 3: Commit**

```powershell
git add src/es8311.c src/es8311.h src/es8311_reg.h
git commit -m @'
feat: vendor ES8311 codec driver from 4B demo

Copy es8311.c/.h/_reg.h (Espressif ES8311 driver, extern "C" guarded)
from the Waveshare 4B demo 07_ES8311 for use by the audio module.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 3: Audio module (init + async beep)

**Files:**
- Create: `src/audio.h`, `src/audio.cpp`
- Modify: `src/main.cpp` (replace `beep()` stub with `#include "audio.h"`; call `audioInit()` in `setup()`)

- [ ] **Step 1: Create `src/audio.h`**

```cpp
#pragma once
#include <Arduino.h>

// Initialize ES8311 codec + I2S + speaker amp, and spawn the audio task.
// Safe to call once from setup() after displayInit(). On failure the module
// logs to serial and beep() becomes a no-op (audio absence never crashes).
void audioInit();

// Enqueue a tone of `freq` Hz for `dur` ms. Non-blocking: returns immediately,
// the audio task plays it. Silent when settings().volume == 0. Drops the tone
// if the queue is full (never blocks the render loop).
void beep(uint16_t freq, uint16_t dur);
```

- [ ] **Step 2: Create `src/audio.cpp`**

```cpp
#include "audio.h"
#include "display.h"   // expander (amp enable on XCA9554 pin 3)
#include "stats.h"     // settings().volume, settings().sineWave
#include "ESP_I2S.h"
#include "es8311.h"
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// I2S / codec pins (Waveshare 4B, from demo pin_config.h)
#define PIN_BCLK 16
#define PIN_LRCK 7
#define PIN_DOUT 6
#define PIN_DIN  15
#define PIN_MCLK 5
#define AMP_EN_PIN 3            // XCA9554 expander pin driving the speaker amp
#define SAMPLE_RATE 16000

struct ToneJob { uint16_t freq; uint16_t dur; };

static I2SClass     i2s;
static QueueHandle_t toneQ = nullptr;
static bool          audioReady = false;

// Per-volume-level peak amplitude (fraction of int16 full-scale).
static const int16_t AMP[5] = { 0, 4915, 11468, 19660, 32767 };  // 0,.15,.35,.6,1.0

static esp_err_t es8311_codec_init() {
  es8311_handle_t h = es8311_create(0, ES8311_ADDRRES_0);
  if (!h) return ESP_FAIL;
  const es8311_clock_config_t clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = SAMPLE_RATE * 256,
    .sample_frequency = SAMPLE_RATE,
  };
  if (es8311_init(h, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return ESP_FAIL;
  es8311_sample_frequency_config(h, clk.mclk_frequency, clk.sample_frequency);
  es8311_microphone_config(h, false);
  es8311_voice_volume_set(h, 90, NULL);   // fixed codec gain; loudness is set in synth
  return ESP_OK;
}

// Render `job` into I2S in small stereo chunks. Blocks this task (not the loop).
static void playTone(const ToneJob& job) {
  uint8_t vol = settings().volume;
  if (vol == 0 || vol > 4 || job.freq == 0) return;
  int16_t amp = AMP[vol];
  bool sine = settings().sineWave;
  uint32_t total = (uint32_t)SAMPLE_RATE * job.dur / 1000;   // samples
  uint32_t halfP = SAMPLE_RATE / (2u * job.freq);
  if (halfP == 0) halfP = 1;

  static int16_t buf[256 * 2];   // 256 stereo frames
  uint32_t n = 0;
  while (n < total) {
    int frames = 0;
    while (frames < 256 && n < total) {
      int16_t v;
      if (sine) v = (int16_t)(amp * sinf(2.0f * (float)M_PI * job.freq * n / SAMPLE_RATE));
      else      v = ((n / halfP) & 1) ? amp : (int16_t)-amp;
      buf[frames * 2]     = v;   // L
      buf[frames * 2 + 1] = v;   // R
      frames++; n++;
    }
    i2s.write((uint8_t*)buf, frames * 2 * sizeof(int16_t));
  }
  // Short silence tail so the amp settles and tones don't run together.
  memset(buf, 0, sizeof(buf));
  i2s.write((uint8_t*)buf, sizeof(buf));
}

static void audioTask(void*) {
  ToneJob job;
  for (;;) {
    if (xQueueReceive(toneQ, &job, portMAX_DELAY) == pdTRUE) playTone(job);
  }
}

void audioInit() {
  // Enable the speaker amplifier via the IO expander created in displayInit().
  if (expander) {
    expander->pinMode(AMP_EN_PIN, OUTPUT);
    expander->digitalWrite(AMP_EN_PIN, HIGH);
    delay(10);
  }
  i2s.setPins(PIN_BCLK, PIN_LRCK, PIN_DOUT, PIN_DIN, PIN_MCLK);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[audio] I2S init failed");
    return;
  }
  if (es8311_codec_init() != ESP_OK) {
    Serial.println("[audio] ES8311 init failed");
    return;
  }
  toneQ = xQueueCreate(4, sizeof(ToneJob));
  if (!toneQ) { Serial.println("[audio] queue alloc failed"); return; }
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 1, nullptr, 1);
  audioReady = true;
  Serial.println("[audio] ready");
}

void beep(uint16_t freq, uint16_t dur) {
  if (!audioReady || settings().volume == 0) return;
  ToneJob job = { freq, dur };
  xQueueSend(toneQ, &job, 0);   // zero timeout: drop if full, never block
}
```

- [ ] **Step 3: Replace the `beep()` stub in `src/main.cpp`**

Add to the include block (after `#include "stats.h"`, ~line 17):

```cpp
#include "audio.h"
```

Delete the stub at lines 138-139:

```cpp
// No buzzer on this board — beep() is a stub so call sites don't branch.
static void beep(uint16_t, uint16_t) {}
```

- [ ] **Step 4: Call `audioInit()` in `setup()`**

In `setup()`, immediately after `displayInit();` (~line 902 after the diag probe), add:

```cpp
  audioInit();      // ES8311 + I2S + speaker amp; beep() is a no-op until this runs
```

- [ ] **Step 5: Compile**

Run: `& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b`
Expected: `[SUCCESS]`.

- [ ] **Step 6: Flash and verify a beep on device**

Temporarily add `beep(1800, 120);` at the end of `setup()` (after the splash `delay(1800)`), flash, and confirm a tone plays through the speaker at boot. Then **remove that temporary line** (the real call sites come in Task 4) and re-flash.
Expected: an audible beep at boot, scaling with the volume setting; silent when volume is set to `off`.

- [ ] **Step 7: Commit**

```powershell
git add src/audio.h src/audio.cpp src/main.cpp
git commit -m @'
feat: ES8311 audio module with async volume/waveform beep

Add audio.h/.cpp: ES8311 + I2S init, speaker-amp enable via the IO
expander, and a FreeRTOS task + queue that synthesizes square/sine tones
scaled by settings().volume. beep() enqueues non-blocking. Replace the
no-op stub in main.cpp and call audioInit() in setup().

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 4: Re-add beep call sites

Restores the original sound feedback. All calls route through `beep()`, which self-gates on `settings().volume`.

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Prompt-arrival chirp**

In the prompt-change block, inside `if (tama.promptId[0]) { ... }` (currently ~972-980), after `wake();` add:

```cpp
      beep(1200, 80);   // prompt arrival
```

- [ ] **Step 2: Approval grant / deny chirps**

In the `inPrompt` tap branch (currently ~1022-1028), add a beep to each path:

```cpp
        if (approve) {
          beep(2400, 60);
          uint32_t tookS = (millis() - promptArrivedMs) / 1000;
          statsOnApproval(tookS);
          if (tookS < 5) triggerOneShot(P_HEART, 2000);
        } else {
          beep(600, 60);
          statsOnDenial();
        }
```

- [ ] **Step 3: Navigation tap clicks**

In the `G_TAP` dispatch, add `beep(1800, 30);` as the first statement of each navigation branch — `audioOpen`, `resetOpen`, `settingsOpen`, `menuOpen`, `DISP_INFO`, `DISP_PET`, and the final `else` (home-tap cycle). For example:

```cpp
    } else if (audioOpen) {
      beep(1800, 30);
      int r = hitAudio(ty);
      if (r >= 0) { audioSel = r; applyAudio(r); }
    } else if (resetOpen) {
      beep(1800, 30);
      int r = hitReset(ty);
      if (r >= 0) { resetSel = r; applyReset(r); }
    } else if (settingsOpen) {
      beep(1800, 30);
      int r = hitSettings(ty);
      if (r >= 0) { settingsSel = r; applySetting(r); }
    } else if (menuOpen) {
      beep(1800, 30);
      int r = hitMenu(ty);
      if (r >= 0) { menuSel = r; menuConfirm(); }
    } else if (displayMode == DISP_INFO) {
      beep(1800, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(1800, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else {
      beep(1800, 30);
      displayMode = (displayMode + 1) % DISP_COUNT;
      applyDisplayMode();
    }
```

(Note: `applyAudio` already calls `beep(1800, 80)` for its sample tone; the `beep(1800, 30)` nav click fires first on the tap that selects the row, then `applyAudio` plays the sample — this matches the original "click then confirm" feel.)

- [ ] **Step 4: Long-press menu toggle**

In the `G_LONG` branch, add `beep(800, 60);` as the first statement:

```cpp
  if (g == G_LONG) {
    beep(800, 60);
    if (resetOpen) { resetOpen = false; redrawAll(); }
```

- [ ] **Step 5: BLE passkey chirp**

Replace the passkey-wake line (currently ~1111):

```cpp
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
```

- [ ] **Step 6: Compile**

Run: `& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b`
Expected: `[SUCCESS]`.

- [ ] **Step 7: Flash and verify on device**

Flash. Verify: tapping menus clicks (1800 Hz), long-press chirps (800 Hz), a prompt arrival chirps (1200 Hz), approve (2400 Hz) and deny (600 Hz) sound distinct, pairing shows a passkey with a chirp. Set volume to `off` → all silent. Cycle waveform `sqr`/`sin` and confirm the sample beep audibly changes character.

- [ ] **Step 8: Commit**

```powershell
git add src/main.cpp
git commit -m @'
feat: re-add beep call sites (prompt/approve/deny/nav/passkey)

Restore the original M5 sound feedback through the new ES8311 beep():
prompt arrival, approval grant/deny, menu navigation clicks, long-press,
and BLE passkey display. All gated by settings().volume.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 5: IMU module (QMI8658 driver)

**Files:**
- Create: `src/imu.h`, `src/imu.cpp`

- [ ] **Step 1: Create `src/imu.h`**

```cpp
#pragma once
#include <Arduino.h>

// Initialize the QMI8658 accelerometer on the shared Wire bus (SDA 47, SCL 48).
// Call once from setup() after displayInit() (Wire is already begun). Returns
// false on failure; the gesture predicates then return false harmlessly.
bool imuInit();

// Latest accelerometer sample (g). Returns false if no fresh data / not ready.
bool imuRead(float* ax, float* ay, float* az);

// True on a sharp motion spike (running-average magnitude delta). Stateless to
// the caller; maintains its own baseline internally.
bool checkShake();

// True when the device is face-down (z-axis dominant and negative).
bool isFaceDown();
```

- [ ] **Step 2: Create `src/imu.cpp`**

```cpp
#include "imu.h"
#include <Wire.h>
#include "SensorQMI8658.hpp"

static SensorQMI8658 qmi;
static bool  imuOk        = false;
static float shakeBaseline = 1.0f;   // EMA of accel magnitude (g)

// Gesture thresholds — TUNE ON DEVICE (Task 7). The QMI8658 mounting on the 4B
// differs from the M5 MPU6886, so axis signs/levels may need adjustment.
static const float SHAKE_DELTA   = 0.8f;
static const float FACEDOWN_Z    = -0.7f;
static const float FACEDOWN_FLAT =  0.4f;

bool imuInit() {
  imuOk = qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, 47, 48);
  if (!imuOk) { Serial.println("[imu] QMI8658 not found"); return false; }
  qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                          SensorQMI8658::ACC_ODR_1000Hz,
                          SensorQMI8658::LPF_MODE_0);
  qmi.enableAccelerometer();
  Serial.println("[imu] QMI8658 ready");
  return true;
}

bool imuRead(float* ax, float* ay, float* az) {
  if (!imuOk) return false;
  if (!qmi.getDataReady()) return false;
  return qmi.getAccelerometer(*ax, *ay, *az);
}

bool checkShake() {
  float ax, ay, az;
  if (!imuRead(&ax, &ay, &az)) return false;
  float mag = sqrtf(ax * ax + ay * ay + az * az);
  float delta = fabsf(mag - shakeBaseline);
  shakeBaseline = shakeBaseline * 0.95f + mag * 0.05f;
  return delta > SHAKE_DELTA;
}

bool isFaceDown() {
  float ax, ay, az;
  if (!imuRead(&ax, &ay, &az)) return false;
  return az < FACEDOWN_Z && fabsf(ax) < FACEDOWN_FLAT && fabsf(ay) < FACEDOWN_FLAT;
}
```

- [ ] **Step 3: Compile**

Run: `& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b`
Expected: `[SUCCESS]`. (Module compiles but is not yet referenced.)

- [ ] **Step 4: Commit**

```powershell
git add src/imu.h src/imu.cpp
git commit -m @'
feat: QMI8658 IMU module (shake + face-down predicates)

Add imu.h/.cpp wrapping SensorLib's SensorQMI8658: accel init on the
shared Wire bus, imuRead(), and checkShake()/isFaceDown() ported from the
original M5 gestures. Thresholds are placeholders pending on-device tuning.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 6: Gesture state machine + nap render guard

**Files:**
- Modify: `src/main.cpp` (include, globals, `setup()`, loop gesture block, render guard)

- [ ] **Step 1: Include and globals**

Add to the include block (after `#include "audio.h"`):

```cpp
#include "imu.h"
```

Near the other nap/persona globals (after `bool responseSent = false;`, ~line 135), add:

```cpp
bool     napping     = false;
uint32_t napStartMs  = 0;
```

- [ ] **Step 2: Initialize the IMU in `setup()`**

After `bool touchOk = touchInit();` and its log line (~line 925), add:

```cpp
  bool imuOk = imuInit();
  Serial.printf("imu init: %s\n", imuOk ? "ok" : "failed");
```

- [ ] **Step 3: Add the gesture state machine in `loop()`**

Immediately after the passkey block (`lastPasskey = pk;`, ~line 1112) and before the render dispatch (`if (screenOff)`), insert:

```cpp
  // ── IMU gestures: shake → dizzy, face-down → nap ─────────────────────
  static int8_t   faceDownFrames = 0;
  static uint32_t lastImuCheck   = 0;
  if (settings().gestures) {
    if (now - lastImuCheck > 50) {
      lastImuCheck = now;
      if (!menuOpen && !settingsOpen && !audioOpen && !resetOpen && !screenOff
          && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
        triggerOneShot(P_DIZZY, 2000);
        beep(1200, 60);
      }
      bool down = isFaceDown();
      if (down) { if (faceDownFrames < 20)  faceDownFrames++; }
      else      { if (faceDownFrames > -10) faceDownFrames--; }
      if (!napping && faceDownFrames >= 15) {
        napping = true;
        napStartMs = now;
      } else if (napping && faceDownFrames <= -8) {
        napping = false;
        statsOnNapEnd((now - napStartMs) / 1000);
      }
    }
  } else if (napping) {
    // Gestures disabled mid-nap: release so the buddy doesn't stay asleep.
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
  }

  // Face-down nap: paint black once, then skip rendering until face-up.
  static bool napPainted = false;
  if (napping) {
    if (!napPainted) { spr.fillScreen(BLACK); blit(); napPainted = true; }
    return;   // skip render/screen-off logic this frame
  }
  napPainted = false;
```

- [ ] **Step 4: Compile**

Run: `& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b`
Expected: `[SUCCESS]`.

- [ ] **Step 5: Flash and smoke-test on device**

Flash. With gestures `on`: shake the device → buddy goes dizzy + chirp; lay it face-down for ~1s → screen goes black; turn face-up → screen restores. Toggle gestures `off` in settings → neither gesture fires. (Thresholds may be off until Task 7 — that's expected.)

- [ ] **Step 6: Commit**

```powershell
git add src/main.cpp
git commit -m @'
feat: IMU gesture state machine + face-down nap

Poll the QMI8658 at 20 Hz: shake triggers a dizzy one-shot + chirp;
face-down (debounced) enters nap, painting the screen black and pausing
render, accumulating nap stats on exit. Gated by settings().gestures.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 7: On-device gesture tuning

The QMI8658's orientation on the 4B differs from the M5's MPU6886, so the thresholds in `imu.cpp` likely need adjustment. This task is hardware-in-the-loop.

**Files:**
- Modify: `src/imu.cpp` (temporary logging, then threshold constants)

- [ ] **Step 1: Add temporary raw-axis logging**

In `imu.cpp`, temporarily add to the top of `imuRead()` (after the `getAccelerometer` success), or add a throttled print in `checkShake()`/`isFaceDown()`:

```cpp
  static uint32_t _lastLog = 0;
  if (millis() - _lastLog > 200) {
    _lastLog = millis();
    Serial.printf("[imu] ax=%.2f ay=%.2f az=%.2f\n", *ax, *ay, *az);
  }
```

Flash. **Note:** app `Serial` output goes to the native USB CDC, which on this board is *not* the CH343/COM4 port. To read these prints, either (a) connect to the board's native-USB COM port if exposed, or (b) temporarily set `-DARDUINO_USB_CDC_ON_BOOT=0` in `platformio.ini` so `Serial` maps to UART0/COM4 for this tuning session (revert afterward).

- [ ] **Step 2: Observe and record axis behavior**

- Rest flat, face-up: note resting `az` sign/magnitude (~±1.0 g) and that `ax`,`ay` are near 0.
- Flip face-down: note the `az` sign — confirm it's clearly negative (or adjust `FACEDOWN_Z` sign if the QMI mounts inverted).
- Shake: note the peak magnitude delta to size `SHAKE_DELTA`.

- [ ] **Step 3: Adjust thresholds**

Update `SHAKE_DELTA`, `FACEDOWN_Z`, `FACEDOWN_FLAT` in `imu.cpp` to match the observed values. If face-down reads positive `az`, flip the comparison (`az > +0.7f`). Remove the temporary logging from Step 1 and revert any `ARDUINO_USB_CDC_ON_BOOT` change.

- [ ] **Step 4: Compile, flash, and confirm**

Run the build, flash, and confirm: a deliberate shake reliably triggers dizzy without false-firing on normal handling; face-down reliably naps within ~1s and wakes on face-up; gentle tilts do not nap.

- [ ] **Step 5: Commit**

```powershell
git add src/imu.cpp
git commit -m @'
fix: tune QMI8658 gesture thresholds for 4B mounting

Calibrate shake/face-down thresholds against the QMI8658's actual
orientation on the 4B; remove temporary axis logging.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Self-Review

**Spec coverage:**
- ES8311 async beep (task + queue, non-blocking) → Task 3. ✓
- Square/sine synth, volume amplitude scaling → Task 3 (`playTone`, `AMP[]`). ✓
- Speaker amp enable via expander pin 3 → Task 3 (`audioInit`). ✓
- Re-added original call sites → Task 4. ✓
- Volume (off+4), waveform, gesture settings + persistence → Task 1 (stats.h). ✓
- Audio submenu (volume/wave/back) + sample beep on change → Task 1 (`applyAudio`/`drawAudio`/`hitAudio`) + Task 4 note. ✓
- `gesture` top-level row + reworked `drawSettings` → Task 1. ✓
- QMI8658 init + `checkShake`/`isFaceDown` → Task 5. ✓
- Gesture state machine gated on `settings().gestures`, shake→dizzy, face-down→nap → Task 6. ✓
- Black-screen nap visuals + render guard → Task 6. ✓
- Nap stats via `statsOnNapEnd` → Task 6. ✓
- On-device threshold calibration → Task 7. ✓

**Type/name consistency:** `beep(uint16_t,uint16_t)`, `audioInit()`, `imuInit()`/`imuRead()`/`checkShake()`/`isFaceDown()`, settings fields `volume`/`sineWave`/`gestures`, submenu symbols `audioOpen`/`audioSel`/`audioItems`/`AUDIO_N`/`applyAudio`/`drawAudio`/`hitAudio` — all referenced consistently across tasks. ✓

**Dependency order:** Settings fields (Task 1) exist before `audio.cpp` reads them (Task 3); `beep()` stub persists through Tasks 1-2 so sample/nav beeps compile; real `beep()` replaces it in Task 3; gesture code (Task 6) depends on `imu.*` (Task 5) and `beep()` (Task 3). ✓
