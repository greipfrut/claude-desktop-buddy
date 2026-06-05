# Battery Support (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the 4B's hardcoded battery/power stubs with real AXP2101 PMIC reads (voltage, percent, charging, USB), a real PMIC power-off, and real values in the BLE status JSON.

**Architecture:** A new self-contained `src/power.*` module (mirroring `display.*`/`touch.*`/`audio.*`) owns the `XPowersPMU` instance, reads the PMIC over the shared Wire bus on a slow timer, and caches values behind the same getter names `main.cpp` already calls — so every existing call site (battery glyph, info page, BLE `bat{}` status, auto-screen-off, power-off menu action) lights up with no signature changes. No PMIC power rails are reconfigured (reading only), so there is no brown-out risk.

**Tech Stack:** PlatformIO + Arduino-ESP32 (pioarduino), `lewisxhe/XPowersLib` (AXP2101 driver), FreeRTOS/Wire.

**This is Phase 1 of the spec** `docs/superpowers/specs/2026-06-05-480x480-layout-and-battery-design.md`. It is layout-independent and ships on its own. Phase 2 (480×480 native UI) is a separate plan, best written after this lands.

**Verification model:** Embedded firmware, no unit-test framework. Each task is verified by (a) `pio run` compiling clean and (b) explicit on-device observation. Build/flash/monitor (PowerShell, board on COM4):

```powershell
& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b
& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b -t upload --upload-port COM4
```

**Note on CDC:** `platformio.ini` currently carries the diagnostic `ARDUINO_USB_CDC_ON_BOOT=0` (from the audio work, so app `Serial` reaches COM4). Battery is verifiable on-screen and over BLE without app serial, so this plan **leaves `=0` in place** for debugging. The flip back to `=1` is the final step of the *project* (the Phase 2 plan). If you ship battery standalone without doing Phase 2, flip it to `1` first.

---

## File Structure

### New files
| File | Responsibility |
|---|---|
| `src/power.h` | Public power API: `powerInit`, `powerPoll`, battery getters, `powerOff` |
| `src/power.cpp` | AXP2101 (XPowersLib) ownership, cached reads, LiPo voltage fallback, real power-off |

### Modified files
| File | Changes |
|---|---|
| `platformio.ini` | Add `lewisxhe/XPowersLib` to `lib_deps` |
| `src/main.cpp` | Remove battery/power stubs; `#include "power.h"`; `powerInit()` in `setup()`; `powerPoll()` timer in `loop()`; info DEVICE page uses real charging/full |

---

## Task 1: Add the dependency and the power API header

**Files:**
- Modify: `platformio.ini` (the `lib_deps` block)
- Create: `src/power.h`

- [ ] **Step 1: Add XPowersLib to `lib_deps` in `platformio.ini`**

Change the `lib_deps` block (currently ending the env) to add the XPowersLib line:

```ini
lib_deps =
    moononournation/GFX Library for Arduino @ ^1.5.6
    lewisxhe/SensorLib @ ^0.2.6
    lewisxhe/XPowersLib @ ^0.2.4
    bitbank2/AnimatedGIF @ ^2.1.1
    bblanchon/ArduinoJson @ ^7.0.0
```

- [ ] **Step 2: Create `src/power.h`**

```cpp
#pragma once
#include <Arduino.h>

// AXP2101 PMIC battery + power module (Waveshare ESP32-S3-Touch-LCD-4B).
// Owns the single XPowersPMU instance. All reads are cached: call powerPoll()
// on a slow timer from loop(); the getters return the cached snapshot so no
// I2C happens in the render hot path. On init failure the getters return safe
// defaults and powerOff() falls back to a restart.

bool powerInit();              // PMIC begin on shared Wire (SDA47/SCL48); enable ADCs. No rail changes.
void powerPoll();              // refresh cached readings; call ~every 3 s from loop()

int  batteryMilliVolts();      // battery voltage, mV
int  batteryPercent();         // 0..100 (PMIC gauge, LiPo voltage-curve fallback)
int  batteryMilliAmps();       // best-effort; 0 if the PMIC doesn't expose it
bool batteryUsbPresent();      // VBUS present
int  batteryUsbMilliVolts();   // VBUS voltage, mV
bool batteryCharging();        // actively charging
bool batteryFull();            // charge complete
void powerOff();               // real AXP2101 shutdown (wakes on PWRKEY / USB)
```

- [ ] **Step 3: Compile**

Run: `& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b`
Expected: `[SUCCESS]`. (XPowersLib downloads; `power.h` isn't referenced yet but the header compiles.)

- [ ] **Step 4: Commit**

```powershell
git add platformio.ini src/power.h
git commit -m @'
feat: add XPowersLib dep + power module API header

Declare the AXP2101 battery/power API (powerInit/powerPoll/getters/powerOff)
that src/power.cpp will implement and main.cpp will call in place of the stubs.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 2: Implement the AXP2101 driver

**Files:**
- Create: `src/power.cpp`

- [ ] **Step 1: Create `src/power.cpp`**

```cpp
#define XPOWERS_CHIP_AXP2101
#include "power.h"
#include "XPowersLib.h"
#include <Wire.h>

static XPowersPMU pmu;
static bool pmuOk = false;

// Cached readings (safe defaults until the first successful powerPoll()).
static int  cMv   = 4200;
static int  cVbus = 5000;
static int  cPct  = 100;
static int  cMa   = 0;
static bool cUsb  = true;
static bool cChg  = false;
static bool cFull = false;

// LiPo voltage -> percent fallback when the PMIC gauge has no reading.
static int lipoPercent(int mv) {
  static const int V[] = { 3300, 3600, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200 };
  static const int P[] = {    0,   10,   20,   30,   40,   50,   60,   75,   90,  100 };
  if (mv <= V[0]) return 0;
  if (mv >= V[9]) return 100;
  for (int i = 1; i < 10; i++) {
    if (mv < V[i]) return P[i-1] + (P[i] - P[i-1]) * (mv - V[i-1]) / (V[i] - V[i-1]);
  }
  return 100;
}

bool powerInit() {
  pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, 47, 48);
  if (!pmuOk) { Serial.println("[power] AXP2101 not found"); return false; }
  // Enable only the ADC paths we read. Do NOT reconfigure DCDC/LDO rails — the
  // board is already powered through them; touching them could brown out the
  // display/ESP. (The 4B AXP2101 demo behaves the same way.)
  pmu.enableBattDetection();
  pmu.enableBattVoltageMeasure();
  pmu.enableVbusVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
  Serial.println("[power] AXP2101 ready");
  powerPoll();
  return true;
}

void powerPoll() {
  if (!pmuOk) return;
  cUsb  = pmu.isVbusIn();
  cChg  = pmu.isCharging();
  cMv   = pmu.getBattVoltage();    // mV
  cVbus = pmu.getVbusVoltage();    // mV
  if (pmu.isBatteryConnect()) {
    int p = pmu.getBatteryPercent();             // -1 when the gauge has no value
    cPct  = (p >= 0 && p <= 100) ? p : lipoPercent(cMv);
  } else {
    cPct  = cUsb ? 100 : lipoPercent(cMv);
  }
  cFull = (pmu.getChargerStatus() == XPOWERS_AXP2101_CHG_DONE_STATE);
  cMa   = 0;   // AXP2101 battery current isn't exposed reliably via XPowersLib
}

int  batteryMilliVolts()    { return cMv; }
int  batteryPercent()       { return cPct; }
int  batteryMilliAmps()     { return cMa; }
bool batteryUsbPresent()    { return cUsb; }
int  batteryUsbMilliVolts() { return cVbus; }
bool batteryCharging()      { return cChg; }
bool batteryFull()          { return cFull; }

void powerOff() {
  if (pmuOk) pmu.shutdown();   // power down PMIC rails; wakes on PWRKEY / USB insert
  else       ESP.restart();    // no PMIC -> safe fallback
}
```

- [ ] **Step 2: Compile**

Run: `& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b`
Expected: `[SUCCESS]`. If the linker complains about a missing `XPOWERS_AXP2101_CHG_DONE_STATE` or method name, the installed XPowersLib version differs — check `~/.platformio/.../XPowersLib` headers for the exact enum/method spelling and adjust. (`power.cpp` compiles but is not referenced yet.)

- [ ] **Step 3: Commit**

```powershell
git add src/power.cpp
git commit -m @'
feat: AXP2101 battery driver (XPowersLib)

Cached reads of battery voltage/percent/charging/USB on the shared Wire bus
(ADC-only init, no rail changes) with a LiPo voltage-curve percent fallback,
plus a real PMIC shutdown for powerOff(). Not yet wired into main.cpp.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Task 3: Wire the module into main.cpp (remove stubs)

**Files:**
- Modify: `src/main.cpp:17` (include), `src/main.cpp:38-53` (stubs), `setup()` (~`main.cpp:955`), `loop()` (~`main.cpp:1000`), info DEVICE page (~`main.cpp:551-563`)

- [ ] **Step 1: Add the include**

In the include block (after `#include "audio.h"`, line 18) add:

```cpp
#include "power.h"
```

- [ ] **Step 2: Delete the battery/power stubs**

Remove lines 38-53 entirely (the `Battery + power stubs` comment block, the five `battery*()` stub functions, and the `static void powerOff()` restart stub). `power.cpp` now provides all of these (and `powerOff()` is no longer `static` — it's the external one from `power.h`, which is fine; the only caller is the reset-submenu action).

- [ ] **Step 3: Initialize the PMIC in `setup()`**

Immediately after `audioInit();` (line 955) add:

```cpp
  bool pmuOk = powerInit();   // AXP2101 battery/power; getters degrade safely if absent
  Serial.printf("power init: %s\n", pmuOk ? "ok" : "failed");
```

- [ ] **Step 4: Poll the PMIC on a timer in `loop()`**

Immediately after `uint32_t now = millis();` (line 1000, the first lines of `loop()`) add:

```cpp
  static uint32_t lastPowerPoll = 0;
  if (now - lastPowerPoll > 3000) { lastPowerPoll = now; powerPoll(); }
```

- [ ] **Step 5: Use the real charging/full state on the info DEVICE page**

Replace the voltage-heuristic derivation at lines 554-555:

```cpp
    bool charging = usb && vBat_mV < 4100;
    bool full = usb && vBat_mV >= 4100;
```

with the real PMIC state:

```cpp
    bool charging = batteryCharging();
    bool full = batteryFull();
```

(The surrounding `int vBat_mV = batteryMilliVolts(); int pct = batteryPercent(); bool usb = batteryUsbPresent();` lines stay — they now return real values.)

- [ ] **Step 6: Compile**

Run: `& "C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe" run -e waveshare-s3-touch-lcd-4b`
Expected: `[SUCCESS]`. No `batteryMilliVolts`/`powerOff` redefinition errors (the stubs are gone; `power.cpp` provides them).

- [ ] **Step 7: Flash and verify on device**

Flash, then open the **Info** screen and page to **DEVICE** (tap home to cycle to info, tap to page to "DEVICE"). Verify, with the device on USB:
- Percent and voltage are **real** (e.g. `4.0x V`, not a fixed `4.20V`/`100%`), and the state word reflects reality (`chrg`/`full`/`bat`/`usb`).
- Pull the USB cable (run from battery): within ~3 s the voltage tracks the battery and the state shows `bat`; reconnect USB → returns to `chrg`/`full`.
- Serial (`power init: ok`) confirms the PMIC was found.

Then verify **power-off**: long-press → menu → **reset** submenu → the power-off action. The device should **fully power down** (screen dark, no auto-reboot). Press the **PWRKEY** (power button) or re-insert USB to boot it back up.

Then verify **BLE status**: with Claude Desktop connected (or via the status JSON on serial), the `bat{}` field reports the real `pct`/`mV`/`usb` instead of `100`/`4200`/`true`.

- [ ] **Step 8: Commit**

```powershell
git add src/main.cpp
git commit -m @'
feat: wire AXP2101 battery/power into the firmware

Remove the battery/power stubs; init the PMIC in setup(), poll it every 3 s in
loop(), and use the real charging/full state on the info DEVICE page. The
on-screen battery glyph, info page, BLE bat{} status, auto-screen-off, and the
power-off menu action now reflect real hardware.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
'@
```

---

## Self-Review

**Spec coverage (Phase 1 portion of the spec):**
- New `src/power.h`/`power.cpp` AXP2101 module → Tasks 1-2. ✓
- `powerInit()` (Wire begin, ADC enable, no rail changes) → Task 2 Step 1. ✓
- Cached `powerPoll()` on a ~3 s timer (no I2C in hot path) → Task 2 + Task 3 Step 4. ✓
- Percent via PMIC gauge with LiPo voltage fallback → Task 2 (`lipoPercent`). ✓
- Charging/full/USB getters → Task 2; consumed on info page → Task 3 Step 5. ✓
- Real `powerOff()` (PMIC shutdown) → Task 2; existing menu caller unchanged. ✓
- BLE `bat{}` reports real values → automatic via the getters (verified Task 3 Step 7). ✓
- `batteryUsbMilliVolts()` kept for API parity → Task 2. ✓
- XPowersLib dependency added → Task 1. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete code; the one on-device action (power-off, threshold observation) has explicit expected behavior. ✓

**Type consistency:** getter names/signatures in `power.h` match `power.cpp` definitions and the existing `main.cpp` call sites (`batteryMilliVolts`/`batteryPercent`/`batteryMilliAmps`/`batteryUsbPresent`/`batteryUsbMilliVolts`/`powerOff`); new `batteryCharging`/`batteryFull` are declared, defined, and consumed consistently. ✓

**Out of scope (Phase 2 plan):** canvas 480×480, touch 1:1, buddy scaling, screen redesigns, drag-scroll gesture, and the `ARDUINO_USB_CDC_ON_BOOT` flip back to 1.
