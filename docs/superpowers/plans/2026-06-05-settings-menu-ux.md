# Settings & Menu UX Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Trim the main menu to 3 items, and redesign settings as a compact bottom-anchored sheet so the pet animates at full scale behind it.

**Architecture:** All changes are in `src/main.cpp`. The menu is simplified by removing indices and remapping `menuConfirm()`. Settings becomes a scrollable 5-row viewport bottom-sheet with drag-to-scroll, and the buddy renders at 3x (home scale) behind it. Audio and reset sub-panels are also bottom-anchored to match.

**Tech Stack:** Arduino/PlatformIO on ESP32-S3, Arduino_GFX display library. Build with `C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run`. Device on COM4.

**Key constants:** `W = 480`, `H = 480` (full 480x480 canvas). Buddy body at 3x occupies roughly y=120..240. Settings items: 9 at 36px each. Menu items: 3 at 44px each.

---

### Task 1: Trim Main Menu to 3 Items

**Files:**
- Modify: `src/main.cpp:164-165` (menuItems, MENU_N)
- Modify: `src/main.cpp:374-389` (menuConfirm)
- Modify: `src/main.cpp:391-408` (drawMenu)

- [ ] **Step 1: Update menu items array and count**

Replace lines 164-165:

```cpp
const char* menuItems[] = { "settings", "about", "close" };
const uint8_t MENU_N = 3;
```

- [ ] **Step 2: Remap menuConfirm() indices**

Replace the entire `menuConfirm()` function (lines 374-389):

```cpp
void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 2: menuOpen = false; redrawAll(); break;
  }
}
```

- [ ] **Step 3: Remove demo label from drawMenu()**

Replace the `drawMenu()` function (lines 391-408):

```cpp
void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 360, mh = 20 + MENU_N * 44 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 8, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 8, p.textDim);
  spr.setTextSize(3);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 12, my + 18 + i * 44);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 14);
  spr.setTextSize(1);
}
```

The only change from the original is removing the `if (i == 4)` demo label line. The rest is identical — `MENU_N` is now 3, so the panel auto-sizes.

- [ ] **Step 4: Build**

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run
```

Expected: clean build, no errors. `hitMenu()` does not need changes — it already uses `MENU_N` for bounds.

- [ ] **Step 5: Flash and verify**

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run -t upload --upload-port COM4
```

Verify on device:
- Long-press opens menu with 3 items: settings, about, close
- Tap "settings" opens settings panel
- Tap "about" shows credits info page
- Tap "close" dismisses menu
- Menu panel is noticeably shorter than before

- [ ] **Step 6: Commit**

```powershell
git add src/main.cpp
git commit -m "feat: trim main menu to settings/about/close

Remove turn-off (physical button exists), demo (debug tool),
and help (reachable via info page tap cycle).

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 2: Compact Bottom-Sheet Settings with Live Pet Preview

**Files:**
- Modify: `src/main.cpp:167-168` (add scroll state variables)
- Modify: `src/main.cpp:273-322` (drawSettings — complete rewrite)
- Modify: `src/main.cpp:1113-1118` (hitSettings — rewrite for bottom-anchor + scroll)
- Modify: `src/main.cpp:374-376` (menuConfirm case 0 — add peek + scroll reset)
- Modify: `src/main.cpp:186-200` (applySetting case 8 — restore peek on close)
- Modify: `src/main.cpp:1287-1289` (G_DRAG handler — add settings scroll)
- Modify: `src/main.cpp:1293-1300` (G_LONG handler — restore peek when closing settings)

- [ ] **Step 1: Add scroll state variables**

After line 168 (`uint8_t settingsSel = 0;`), add:

```cpp
static uint8_t settingsScrollOff = 0;
static int16_t settingsDragAccum = 0;
static const uint8_t SETTINGS_VISIBLE = 5;
```

- [ ] **Step 2: Rewrite drawSettings() as bottom-anchored scrollable sheet**

Replace the entire `drawSettings()` function (lines 273-322):

```cpp
static void drawSettings() {
  const Palette& p = characterPalette();

  if (settingsSel < settingsScrollOff) settingsScrollOff = settingsSel;
  if (settingsSel >= settingsScrollOff + SETTINGS_VISIBLE)
    settingsScrollOff = settingsSel - SETTINGS_VISIBLE + 1;

  int mw = 360;
  int mh = 16 + SETTINGS_VISIBLE * 36 + MENU_HINT_H;
  int mx = (W - mw) / 2;
  int my = H - mh;
  spr.fillRoundRect(mx, my, mw, mh, 8, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 8, p.textDim);
  spr.setTextSize(3);
  Settings& s = settings();
  for (int v = 0; v < SETTINGS_VISIBLE; v++) {
    int i = v + settingsScrollOff;
    if (i >= SETTINGS_N) break;
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 10, my + 14 + v * 36);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setTextSize(2);
    spr.setCursor(mx + mw - 80, my + 18 + v * 36);
    spr.setTextColor(p.textDim, PANEL);
    switch (i) {
      case 0: spr.printf("%u/5", s.bright + 1); break;
      case 1: {
        static const char* SOF_LABELS[] = { "off", " 30s", "  1m", "  2m", "  5m" };
        spr.print(SOF_LABELS[s.screenOff]);
        break;
      }
      case 2:
        if (s.volume == 0) spr.print("off");
        else               spr.printf("%u/4", s.volume);
        break;
      case 3: spr.setTextColor(s.bt ? GREEN : p.textDim, PANEL);
              spr.print(s.bt ? " on" : "off"); break;
      case 4:
        if (!s.bt)                  { spr.print("off"); }
        else if (bleConnected())    { spr.setTextColor(GREEN, PANEL); spr.print(" ok"); }
        else                        { spr.setTextColor(HOT, PANEL);   spr.print(" go"); }
        break;
      case 5: spr.setTextColor(s.hud ? GREEN : p.textDim, PANEL);
              spr.print(s.hud ? " on" : "off"); break;
      case 6: {
        uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
        uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
        spr.printf("%u/%u", pos, total);
        break;
      }
      default: break;
    }
    spr.setTextSize(3);
  }

  if (SETTINGS_N > SETTINGS_VISIBLE) {
    int trackX = mx + mw - 10, trackY = my + 10;
    int trackH = SETTINGS_VISIBLE * 36;
    int barH = trackH * SETTINGS_VISIBLE / SETTINGS_N;
    if (barH < 10) barH = 10;
    int maxOff = SETTINGS_N - SETTINGS_VISIBLE;
    int barY = trackY + (trackH - barH) * settingsScrollOff / maxOff;
    spr.fillRect(trackX, barY, 4, barH, p.textDim);
  }

  drawMenuHints(p, mx, mw, my + mh - 14);
  spr.setTextSize(1);
}
```

Key differences from the original:
- `my = H - mh` (bottom-anchored, was centered)
- Loop uses visible index `v` mapped to item index `i = v + settingsScrollOff`
- Panel shows only `SETTINGS_VISIBLE` (5) rows at a time
- Scrollbar drawn when total items exceed visible count
- Value column shifted slightly left to `mx + mw - 80` to clear the scrollbar

- [ ] **Step 3: Rewrite hitSettings() for bottom-anchor + scroll offset**

Replace `hitSettings()` (lines 1113-1118):

```cpp
static int hitSettings(int ty) {
  int mh = 16 + SETTINGS_VISIBLE * 36 + MENU_HINT_H;
  int my = H - mh;
  int firstY = my + 14;
  int visRow = (ty - firstY) / 36;
  if (visRow < 0 || visRow >= SETTINGS_VISIBLE) return -1;
  int i = visRow + settingsScrollOff;
  return (i < SETTINGS_N) ? i : -1;
}
```

- [ ] **Step 4: Add drag-to-scroll in the G_DRAG handler**

Find the `G_DRAG` handler block (lines 1287-1289):

```cpp
  if (g == G_DRAG) {
    if (inPrompt) hintScroll -= gDragDy;
    else if (displayMode == DISP_TRANSCRIPT) transcriptScroll -= gDragDy;
  }
```

Replace with:

```cpp
  if (g == G_DRAG) {
    if (settingsOpen && !audioOpen && !resetOpen) {
      settingsDragAccum -= gDragDy;
      int maxOff = SETTINGS_N - SETTINGS_VISIBLE;
      while (settingsDragAccum >= 36 && settingsScrollOff < maxOff) {
        settingsScrollOff++;
        settingsDragAccum -= 36;
      }
      while (settingsDragAccum <= -36 && settingsScrollOff > 0) {
        settingsScrollOff--;
        settingsDragAccum += 36;
      }
      if (settingsScrollOff == 0 && settingsDragAccum < 0) settingsDragAccum = 0;
      if (settingsScrollOff >= maxOff && settingsDragAccum > 0) settingsDragAccum = 0;
    } else if (inPrompt) {
      hintScroll -= gDragDy;
    } else if (displayMode == DISP_TRANSCRIPT) {
      transcriptScroll -= gDragDy;
    }
  }
```

The accumulator converts pixel drag deltas into row-sized scroll steps. Edge clamping prevents the accumulator from building up at scroll boundaries.

- [ ] **Step 5: Add peek + scroll reset to menuConfirm() case 0**

Replace the `case 0:` line in `menuConfirm()` (which was updated in Task 1):

```cpp
    case 0:
      settingsOpen = true; menuOpen = false; settingsSel = 0;
      settingsScrollOff = 0; settingsDragAccum = 0;
      buddySetPeek(false); characterSetPeek(false);
      buddyInvalidate(); characterInvalidate();
      break;
```

This forces the buddy to render at 3x (home scale) regardless of which `displayMode` the user was in when they opened the menu.

- [ ] **Step 6: Restore peek when settings closes via "back"**

Replace `applySetting()` case 8 (line 197):

```cpp
    case 8: {
      settingsOpen = false;
      characterSetPeek(displayMode != DISP_NORMAL);
      buddySetPeek(displayMode != DISP_NORMAL);
      redrawAll();
      return;
    }
```

- [ ] **Step 7: Restore peek when settings closes via long-press**

Find the G_LONG handler's `settingsOpen` branch (around line 1295):

```cpp
    else if (settingsOpen) { settingsOpen = false; redrawAll(); }
```

Replace with:

```cpp
    else if (settingsOpen) {
      settingsOpen = false;
      characterSetPeek(displayMode != DISP_NORMAL);
      buddySetPeek(displayMode != DISP_NORMAL);
      redrawAll();
    }
```

- [ ] **Step 8: Build**

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run
```

Expected: clean build.

- [ ] **Step 9: Flash and verify**

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run -t upload --upload-port COM4
```

Verify on device:
- Open settings: compact panel at bottom of screen, pet animates at full size above
- Tap "pet" repeatedly: species changes immediately, visible behind the panel
- Drag up/down in settings: rows scroll, scrollbar moves
- Scroll to bottom: "reset" and "back" visible
- Tap "back" or long-press: settings closes, buddy returns to correct scale
- Open settings from a non-home display mode (e.g., long-press while on info page): pet still shows at full 3x scale
- Close settings: buddy returns to peek scale matching the current display mode

- [ ] **Step 10: Commit**

```powershell
git add src/main.cpp
git commit -m "feat: compact bottom-sheet settings with live pet preview

Settings panel becomes a scrollable 5-row bottom-anchored sheet.
The pet animates at full 3x scale behind it, updating immediately
when the species is changed. Drag to scroll through all 9 items.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 3: Bottom-Anchor Audio and Reset Sub-Panels

**Files:**
- Modify: `src/main.cpp:324-351` (drawAudio — change my calculation)
- Modify: `src/main.cpp:353-372` (drawReset — change my calculation)
- Modify: `src/main.cpp:1128-1134` (hitAudio — change my calculation)
- Modify: `src/main.cpp:1120-1126` (hitReset — change my calculation)

- [ ] **Step 1: Bottom-anchor drawAudio()**

In `drawAudio()`, change the `my` calculation (line 328). Replace:

```cpp
  int mw = 360, mh = 16 + AUDIO_N * 36 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
```

With:

```cpp
  int mw = 360, mh = 16 + AUDIO_N * 36 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = H - mh;
```

- [ ] **Step 2: Bottom-anchor drawReset()**

In `drawReset()`, change the `my` calculation (line 356). Replace:

```cpp
  int mw = 360, mh = 20 + RESET_N * 44 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
```

With:

```cpp
  int mw = 360, mh = 20 + RESET_N * 44 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = H - mh;
```

- [ ] **Step 3: Bottom-anchor hitAudio()**

Replace `hitAudio()`:

```cpp
static int hitAudio(int ty) {
  int mh = 16 + AUDIO_N * 36 + MENU_HINT_H;
  int my = H - mh;
  int firstY = my + 14;
  int i = (ty - firstY) / 36;
  return (i >= 0 && i < AUDIO_N) ? i : -1;
}
```

- [ ] **Step 4: Bottom-anchor hitReset()**

Replace `hitReset()`:

```cpp
static int hitReset(int ty) {
  int mh = 20 + RESET_N * 44 + MENU_HINT_H;
  int my = H - mh;
  int firstY = my + 18;
  int i = (ty - firstY) / 44;
  return (i >= 0 && i < RESET_N) ? i : -1;
}
```

- [ ] **Step 5: Build**

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run
```

Expected: clean build.

- [ ] **Step 6: Flash and verify**

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run -t upload --upload-port COM4
```

Verify on device:
- Settings > sound: audio panel appears bottom-anchored, pet visible above
- Tap volume/wave: settings cycle correctly
- Tap back: returns to settings
- Settings > reset: reset panel appears bottom-anchored with red border
- Tap "del char" or "factory": shows "really?" confirmation (tap again within 3s to execute)
- Tap back: returns to settings

- [ ] **Step 7: Commit**

```powershell
git add src/main.cpp
git commit -m "feat: bottom-anchor audio and reset sub-panels

Match the new settings bottom-sheet layout so all overlay panels
sit at the bottom of the screen with the pet visible above.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```
