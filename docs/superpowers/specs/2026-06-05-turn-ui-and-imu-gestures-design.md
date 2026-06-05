# evt:turn UI Display + IMU Gestures Design

## Feature 1: evt:turn UI Display

### Problem

The BLE protocol sends `evt:turn` events with actual Claude output text (~119 chars), but nothing displays them. The transcript page only shows `entries[]` from heartbeats, which the desktop truncates at ~90 chars. Turn events carry richer content that goes unused.

### Design

Two rendering changes, no new files or state:

**Home bottom bar (drawStatusBars, line 2):** When `lastTurnMs` is recent (< 60s from now), replace the 2nd transcript line with `"Claude: <snippet>"` (or `"You: <snippet>"` for user turns) in `p.body` color. After 60s, fall back to showing `entries[1]`. Truncated to screen width like entries already are.

**Transcript page (drawTranscript, prepended entry):** When `lastTurnSnippet[0] != 0`, render it as a special "entry 0" before the heartbeat entries array. Left border uses `p.body` color (distinct from the gradient). Text shows `"[assistant] <snippet>"` or `"[user] <snippet>"`. Word-wrapped identically to regular entries.

### Data flow

No parser changes. `lastTurnRole`, `lastTurnSnippet` (119 chars), `lastTurnMs` are already populated by `_applyJson()` in `src/data.h:94-117`.

### Files modified

| File | Changes |
|---|---|
| `src/main.cpp` | `drawStatusBars()`: conditional line 2 replacement. `drawTranscript()`: prepend turn entry before entries loop. |

---

## Feature 2: IMU Gestures (QMI8658)

### Problem

The QMI8658 6-axis IMU on the 4B board is unused. The original M5StickC buddy had shake-to-dizzy gestures via MPU6886. The `gestures` setting and `statsOnNapEnd()` already exist but nothing drives them.

### Design

Two new files (`imu.h`, `imu.cpp`) following the `display.*`/`touch.*`/`audio.*` driver module pattern. Main.cpp polls the IMU at 20Hz and runs a gesture state machine.

**imu.h public API:**
- `bool imuInit()` -- init QMI8658 on shared Wire bus (SDA=47, SCL=48)
- `bool imuRead(float* ax, float* ay, float* az)` -- latest accel sample
- `bool checkShake()` -- EMA-based magnitude delta detection
- `bool isFaceDown()` -- z-axis dominant negative check

**imu.cpp internals:**
- SensorQMI8658 from SensorLib (already a dependency)
- Shake: maintain EMA baseline of accel magnitude, fire when delta > threshold
- Face-down: z < -0.7g with x,y near zero (flat)
- Thresholds are constants, tuned on-device

**main.cpp gesture state machine:**
- Poll every 50ms when `settings().gestures` is true
- Shake (outside menus, no active one-shot): `triggerOneShot(P_DIZZY, 2000)` + `beep(1200, 60)`
- Face-down debounce: 15 consecutive "down" frames to enter nap, 8 consecutive "up" frames to exit
- Nap mode: fill screen black once, skip all rendering, call `statsOnNapEnd()` on exit
- Disabling gestures mid-nap releases the nap state

### Files

| File | Changes |
|---|---|
| `src/imu.h` | New: public IMU API |
| `src/imu.cpp` | New: QMI8658 driver + gesture predicates |
| `src/main.cpp` | `#include "imu.h"`, `imuInit()` in setup, gesture state machine + nap guard in loop |

### Dependencies

- `lewisxhe/SensorLib @ ^0.2.6` (already in platformio.ini for touch)
- Shared Wire bus (already initialized by displayInit)
- `settings().gestures` (already in stats.h)
- `statsOnNapEnd()` (already in stats.h)
- `beep()` (already in audio.h)
