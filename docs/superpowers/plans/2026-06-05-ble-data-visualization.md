# BLE Data Visualization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show more of what Claude Desktop sends over BLE on the 480x480 touchscreen -- bigger buffers, transcript view, token counter, AskUserQuestion handling, turn event parsing.

**Architecture:** All changes are in two files: `src/data.h` (parser + state struct) and `src/main.cpp` (UI rendering + touch dispatch). The data layer (data.h) is modified first so the UI changes can immediately use the new fields. No new files are created.

**Tech Stack:** C++ / Arduino framework / PlatformIO / ESP32-S3 / Arduino_GFX display library

**Build:** `C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run` (PowerShell only). Incremental builds: 15-60s (warm cache). Do NOT wipe `.pio/build/`.

**Flash + monitor:** `pio run -t upload` then `pio run -t monitor` (COM4). CDC_ON_BOOT=1 in production; flip to 0 in `platformio.ini` `board_build.extra_flags` for serial debug output.

---

### Task 1: Enlarge data buffers

**Files:**
- Modify: `src/data.h:8-23` (TamaState struct)
- Modify: `src/data.h:96-117` (_applyJson parser)

- [ ] **Step 1: Widen TamaState buffer fields**

In `src/data.h`, change the TamaState struct fields:

```cpp
// Before:
  char     msg[24];
  // ...
  char     promptTool[20];
  char     promptHint[44];

// After:
  char     msg[100];
  // ...
  char     promptTool[48];
  char     promptHint[256];
```

- [ ] **Step 2: Update strncpy bounds in _applyJson to use sizeof()**

In `_applyJson()`, the `msg` copy on line 97 currently uses a hardcoded size. Change all three to use `sizeof()`:

```cpp
// msg (around line 97):
// Before:
  if (m) { strncpy(out->msg, m, sizeof(out->msg)-1); out->msg[sizeof(out->msg)-1]=0; }
// This one already uses sizeof() -- no change needed. Verify it does.

// promptTool and promptHint (around line 114-117):
// Before:
    strncpy(out->promptTool, pt  ? pt  : "", sizeof(out->promptTool)-1); out->promptTool[sizeof(out->promptTool)-1]=0;
    strncpy(out->promptHint, ph  ? ph  : "", sizeof(out->promptHint)-1); out->promptHint[sizeof(out->promptHint)-1]=0;
// These already use sizeof() -- verify all three use sizeof(out->field)-1, not hardcoded numbers.
```

Also check the `snprintf` in `dataPoll()` around line 152:
```cpp
// Before:
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
// Already uses sizeof() -- no change needed.
```

- [ ] **Step 3: Build to verify no compile errors**

Run (PowerShell):
```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run
```

Expected: BUILD SUCCESS. The struct is larger but nothing else changes.

- [ ] **Step 4: Commit**

```powershell
git add src/data.h
git commit -m "feat: enlarge msg/promptHint/promptTool buffers

msg 24->100, promptHint 44->256, promptTool 20->48.
Prevents truncation of real Claude Desktop data."
```

---

### Task 2: Add evt:turn parser to data.h

**Files:**
- Modify: `src/data.h:8-23` (TamaState struct -- add turn fields)
- Modify: `src/data.h:71-123` (_applyJson -- add turn event parsing)

- [ ] **Step 1: Add turn event fields to TamaState**

Add these fields to the TamaState struct, after the `promptHint` field:

```cpp
  char     lastTurnRole[12];     // "assistant" or "user"
  char     lastTurnSnippet[120]; // first text block, truncated
  uint32_t lastTurnMs;           // millis() when received
```

- [ ] **Step 2: Add turn event parsing in _applyJson()**

At the top of `_applyJson()`, after the `xferCommand()` check and the time sync block, but *before* the heartbeat field parsing (the `out->sessionsTotal = ...` block), add:

```cpp
  // Turn events: {"evt":"turn","role":"...","content":[{"type":"text","text":"..."}]}
  const char* evt = doc["evt"];
  if (evt && strcmp(evt, "turn") == 0) {
    const char* role = doc["role"];
    if (role) {
      strncpy(out->lastTurnRole, role, sizeof(out->lastTurnRole)-1);
      out->lastTurnRole[sizeof(out->lastTurnRole)-1] = 0;
    }
    out->lastTurnSnippet[0] = 0;
    JsonArray content = doc["content"];
    if (!content.isNull()) {
      for (JsonVariant block : content) {
        const char* btype = block["type"];
        if (btype && strcmp(btype, "text") == 0) {
          const char* txt = block["text"];
          if (txt) {
            strncpy(out->lastTurnSnippet, txt, sizeof(out->lastTurnSnippet)-1);
            out->lastTurnSnippet[sizeof(out->lastTurnSnippet)-1] = 0;
          }
          break;
        }
      }
    }
    out->lastTurnMs = millis();
    _lastLiveMs = millis();
    return;
  }
```

The `return` at the end is important -- turn events don't carry heartbeat fields, so skip the rest of the parser.

- [ ] **Step 3: Build to verify**

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run
```

Expected: BUILD SUCCESS.

- [ ] **Step 4: Commit**

```powershell
git add src/data.h
git commit -m "feat: parse evt:turn events into TamaState

Stores lastTurnRole, lastTurnSnippet (first text block, 119 chars),
and lastTurnMs. No UI display yet -- data available for future use."
```

---

### Task 3: Expand home bottom bar with transcript preview + token counter

**Files:**
- Modify: `src/main.cpp:821-861` (drawStatusBars function)

- [ ] **Step 1: Add token formatting helper**

Add this helper function above `drawStatusBars()` (before line 821):

```cpp
static void fmtTokens(char* buf, size_t sz, uint32_t v) {
  if (v >= 1000000) snprintf(buf, sz, "%lu.%luM", v/1000000, (v/100000)%10);
  else if (v >= 1000) snprintf(buf, sz, "%lu.%luK", v/1000, (v/100)%10);
  else snprintf(buf, sz, "%lu", (unsigned long)v);
}
```

- [ ] **Step 2: Rewrite drawStatusBars() bottom section**

Replace the entire bottom bar section of `drawStatusBars()` (the block starting with `// -- Bottom bar`). The top bar stays unchanged. New bottom bar:

```cpp
  // ── Bottom bar (y H-80..H): sessions/tokens line + 2 transcript lines ──
  spr.fillRect(0, H - 80, W, 80, p.bg);
  spr.setTextSize(2);

  // Line 1: session counts (left) + tokens_today (right)
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, H - 76);
  spr.printf("%u running  %u waiting  lv %u",
    tama.sessionsRunning, tama.sessionsWaiting, stats().level);

  if (tama.tokensToday > 0) {
    char tokBuf[12];
    fmtTokens(tokBuf, sizeof(tokBuf), tama.tokensToday);
    int16_t bx, by; uint16_t tw, th;
    spr.getTextBounds(tokBuf, 0, 0, &bx, &by, &tw, &th);
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 8 - tw, H - 76);
    spr.print(tokBuf);
  }

  // Lines 2-3: two most recent transcript entries
  for (int i = 0; i < 2 && i < tama.nLines; i++) {
    spr.setTextColor(i == 0 ? p.textDim : PANEL, p.bg);
    spr.setCursor(8, H - 56 + i * 20);
    spr.print(tama.lines[i]);
  }

  spr.setTextSize(1);
```

Note: the second transcript line uses `PANEL` (0x2104, very dim gray) to create a visual fade. If only 0 or 1 lines exist, the loop naturally stops.

- [ ] **Step 3: Build to verify**

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run
```

Expected: BUILD SUCCESS.

- [ ] **Step 4: Flash and verify on device**

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run -t upload
```

Connect to Claude Desktop over BLE. Verify:
- Token counter appears right-aligned on the bottom bar first line
- Two transcript lines appear below the session counts
- Buddy is not cut off (should have ~360px of vertical space)

- [ ] **Step 5: Commit**

```powershell
git add src/main.cpp
git commit -m "feat: show transcript preview + token counter on home screen

Bottom bar grows from 40px to 80px. First line shows session counts
(left) and tokens_today formatted as K/M (right). Two most recent
transcript entries shown below."
```

---

### Task 4: Add dedicated transcript page (DISP_TRANSCRIPT)

**Files:**
- Modify: `src/main.cpp:75` (DisplayMode enum)
- Modify: `src/main.cpp:80-83` (add transcriptScroll global)
- Modify: `src/main.cpp:863-867` (drawHUD -- lineGen reset)
- Add function: `src/main.cpp` (new drawTranscript function, before drawHUD)
- Modify: `src/main.cpp:1113` (G_DRAG handler -- add transcript scroll)
- Modify: `src/main.cpp:1249-1254` (render dispatch -- add transcript case)

- [ ] **Step 1: Update DisplayMode enum**

Change the enum at line 75:

```cpp
// Before:
enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };

// After:
enum DisplayMode { DISP_NORMAL, DISP_TRANSCRIPT, DISP_PET, DISP_INFO, DISP_COUNT };
```

- [ ] **Step 2: Add transcriptScroll global**

Add after the `hintScroll` declaration (around line 83):

```cpp
int      transcriptScroll = 0;
```

- [ ] **Step 3: Update lineGen reset in drawHUD to also reset transcriptScroll**

In `drawHUD()` (around line 865), add `transcriptScroll = 0;` to the lineGen change handler:

```cpp
// Before:
  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

// After:
  if (tama.lineGen != lastLineGen) { msgScroll = 0; transcriptScroll = 0; lastLineGen = tama.lineGen; wake(); }
```

- [ ] **Step 4: Write the drawTranscript function**

Add this function *before* `drawHUD()` (around line 863):

```cpp
static void drawTranscript() {
  const Palette& p = characterPalette();
  spr.fillScreen(p.bg);

  // Top bar (shared)
  spr.fillRect(0, 0, W, 40, p.bg);
  spr.setTextSize(2);
  if (dataRtcValid()) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(8, 12);
    spr.printf("%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  }
  spr.setTextColor(p.text, p.bg);
  drawCenteredString(canvas, petName(), W / 2, 20);
  {
    int pct = batteryPercent();
    bool chrg = batteryCharging();
    char batBuf[12];
    snprintf(batBuf, sizeof(batBuf), chrg ? "%d%%~" : "%d%%", pct);
    int16_t bx1, by1; uint16_t btw, bth;
    spr.getTextBounds(batBuf, 0, 0, &bx1, &by1, &btw, &bth);
    spr.setTextColor(chrg ? GREEN : p.textDim, p.bg);
    spr.setCursor(W - 8 - btw, 12);
    spr.print(batBuf);
  }

  // Summary line (y 40..60): sessions + tokens
  spr.setTextSize(2);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 44);
  spr.printf("%u running  %u waiting  lv %u",
    tama.sessionsRunning, tama.sessionsWaiting, stats().level);
  if (tama.tokensToday > 0) {
    char tokBuf[12];
    fmtTokens(tokBuf, sizeof(tokBuf), tama.tokensToday);
    int16_t bx, by; uint16_t tw, th;
    spr.getTextBounds(tokBuf, 0, 0, &bx, &by, &tw, &th);
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 8 - tw, 44);
    spr.print(tokBuf);
  }

  // Transcript body (y 64..436)
  const int BODY_Y = 64, BODY_H = 372, LINE_H = 22;
  const int WRAP_COLS = 37;
  spr.setTextSize(2);

  // Calculate total height and render
  int totalH = 0;
  int lineHeights[8];
  for (int i = 0; i < tama.nLines; i++) {
    int len = strlen(tama.lines[i]);
    int wrapped = (len + WRAP_COLS - 1) / WRAP_COLS;
    if (wrapped < 1) wrapped = 1;
    lineHeights[i] = wrapped * LINE_H + 6;
    totalH += lineHeights[i];
  }

  int maxScroll = totalH - BODY_H;
  if (maxScroll < 0) maxScroll = 0;
  if (transcriptScroll > maxScroll) transcriptScroll = maxScroll;
  if (transcriptScroll < 0) transcriptScroll = 0;

  int yy = BODY_Y - transcriptScroll;
  char wrapLine[48];
  for (int i = 0; i < tama.nLines; i++) {
    // Color gradient: newest bright, oldest dim
    uint8_t fade = (tama.nLines > 1) ? (i * 255 / (tama.nLines - 1)) : 0;
    uint16_t col = (fade < 128) ? p.text : p.textDim;

    // Left border accent
    int entryTop = yy;
    int entryBot = yy + lineHeights[i] - 6;
    if (entryBot > BODY_Y && entryTop < BODY_Y + BODY_H) {
      int drawTop = max(entryTop, BODY_Y);
      int drawBot = min(entryBot, BODY_Y + BODY_H);
      spr.fillRect(8, drawTop, 2, drawBot - drawTop, p.body);
    }

    // Word-wrapped text
    int len = strlen(tama.lines[i]);
    spr.setTextColor(col, p.bg);
    for (int off = 0; off < len; off += WRAP_COLS) {
      if (yy + LINE_H > BODY_Y && yy < BODY_Y + BODY_H) {
        int take = min((int)(len - off), WRAP_COLS);
        memcpy(wrapLine, tama.lines[i] + off, take);
        wrapLine[take] = 0;
        spr.setCursor(18, yy);
        spr.print(wrapLine);
      }
      yy += LINE_H;
    }
    yy += 6;
  }

  // Scrollbar
  if (maxScroll > 0) {
    int barH = BODY_H * BODY_H / totalH;
    if (barH < 10) barH = 10;
    int barY = BODY_Y + (BODY_H - barH) * transcriptScroll / maxScroll;
    spr.fillRect(W - 6, BODY_Y, 4, BODY_H, p.bg);
    spr.fillRect(W - 6, barY, 4, barH, p.textDim);
  }

  // Bottom hint
  spr.setTextColor(p.textDim, p.bg);
  spr.setTextSize(2);
  drawCenteredString(canvas, "tap: home   hold: menu", W / 2, H - 18);
  spr.setTextSize(1);
}
```

- [ ] **Step 5: Add drag handling for transcript scroll**

In the main loop, find the `G_DRAG` handler (around line 1113):

```cpp
// Before:
  if (g == G_DRAG && inPrompt) { hintScroll -= gDragDy; }

// After:
  if (g == G_DRAG) {
    if (inPrompt) hintScroll -= gDragDy;
    else if (displayMode == DISP_TRANSCRIPT) transcriptScroll -= gDragDy;
  }
```

- [ ] **Step 6: Add transcript to the render dispatch**

In the render dispatch block (around line 1249-1254), add the transcript case:

```cpp
// Before:
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (settings().hud) drawHUD();

// After:
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (displayMode == DISP_TRANSCRIPT) drawTranscript();
    else if (settings().hud) drawHUD();
```

- [ ] **Step 7: Build to verify**

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run
```

Expected: BUILD SUCCESS.

- [ ] **Step 8: Flash and verify on device**

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run -t upload
```

Verify:
- Tap cycle from home goes to transcript page (shows all 8 lines if available)
- Top bar shows clock/name/battery (same as home)
- Summary line shows session counts + token counter
- Transcript lines have left border accent, word-wrap, and color gradient
- Drag scrolls when content overflows
- Scrollbar appears when content is taller than 372px
- Tap from transcript goes to pet page (cycle continues normally)
- New transcript data (lineGen change) resets scroll to top

- [ ] **Step 9: Commit**

```powershell
git add src/main.cpp
git commit -m "feat: add dedicated transcript page (DISP_TRANSCRIPT)

New display mode in tap cycle: home -> transcript -> pet -> info.
Shows all 8 transcript lines with word-wrap, left border accent,
color gradient, drag scrolling, and scrollbar. Summary line shows
session counts and tokens_today."
```

---

### Task 5: AskUserQuestion empirical testing

**Files:**
- Modify: `src/main.cpp:640-706` (drawApproval function)
- Modify: `src/main.cpp:1067-1087` (prompt arrival handler)
- Modify: `src/main.cpp:1128-1149` (tap handler for approval buttons)

This task builds all three AskUserQuestion variants behind a `#define`, then tests each on the real device against Claude Desktop.

- [ ] **Step 1: Add AskUserQuestion mode define and detection**

Add near the top of main.cpp, after the color constants (around line 60):

```cpp
// AskUserQuestion handling mode. Test each, pick winner.
// 0 = auto-approve, 1 = single OK, 2 = better label (default)
#define ASK_USER_MODE 2
```

- [ ] **Step 2: Add isAskUserQuestion helper**

Add before `drawApproval()`:

```cpp
static bool isAskUserQuestion() {
  return strcmp(tama.promptTool, "AskUserQuestion") == 0;
}
```

- [ ] **Step 3: Add auto-approve logic in prompt arrival handler**

In the prompt arrival handler (around line 1074-1087), add auto-approve for mode 0:

```cpp
  if (tama.promptId[0] && strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    hintScroll = 0;
    promptArrivedMs = millis();
    wake();

#if ASK_USER_MODE == 0
    if (isAskUserQuestion()) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd),
               "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}",
               tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      beep(1600, 60);
    } else {
#endif
      beep(1200, 80);
#if ASK_USER_MODE == 0
    }
#endif

    displayMode = DISP_NORMAL;
    menuOpen = settingsOpen = resetOpen = audioOpen = false;
    applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
  }
```

- [ ] **Step 4: Modify drawApproval for AskUserQuestion variants**

In `drawApproval()`, change the header and button rendering:

Replace the header section (lines 644-649):

```cpp
  // Header
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  spr.setTextSize(2);
  spr.setTextColor(waited >= 10 ? HOT : p.textDim, p.bg);
  spr.setCursor(12, 12);
  bool isAUQ = isAskUserQuestion();
#if ASK_USER_MODE == 0
  if (isAUQ && responseSent) {
    spr.print("question pending");
  } else {
    spr.printf("approve? %lus", (unsigned long)waited);
  }
#elif ASK_USER_MODE == 2
  if (isAUQ) {
    spr.print("question for you");
  } else {
    spr.printf("approve? %lus", (unsigned long)waited);
  }
#else
  spr.printf("approve? %lus", (unsigned long)waited);
#endif
```

Replace the tool name section (lines 652-655):

```cpp
  // Tool name or info
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(12, 44);
#if ASK_USER_MODE == 0
  if (isAUQ) {
    spr.setTextSize(2);
    spr.print("answer on desktop");
  } else {
    spr.print(tama.promptTool);
  }
#elif ASK_USER_MODE == 1
  if (isAUQ) {
    spr.setTextSize(2);
    spr.print("answer on desktop");
  } else {
    spr.print(tama.promptTool);
  }
#else
  spr.print(tama.promptTool);
#endif
```

Replace the OK/NO buttons section (lines 693-704):

```cpp
  // Buttons
  int btnY = H - 84, btnH = 76;
  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg); spr.setTextSize(3);
    drawCenteredString(canvas, "sent", W / 2, btnY + btnH / 2);
#if ASK_USER_MODE == 1
  } else if (isAUQ) {
    spr.fillRoundRect(W/4, btnY, W/2, btnH, 12, GREEN);
    spr.setTextSize(4);
    spr.setTextColor(0x0000, GREEN);
    drawCenteredString(canvas, "OK", W/2, btnY + btnH/2);
#endif
  } else {
    spr.fillRoundRect(8,         btnY, W/2 - 12, btnH, 12, GREEN);
    spr.fillRoundRect(W/2 + 4,   btnY, W/2 - 12, btnH, 12, HOT);
    spr.setTextSize(4);
    spr.setTextColor(0x0000, GREEN); drawCenteredString(canvas, "OK", W/4,     btnY + btnH/2);
    spr.setTextColor(0x0000, HOT);   drawCenteredString(canvas, "NO", 3*W/4,   btnY + btnH/2);
  }
  spr.setTextSize(1);
```

- [ ] **Step 5: Modify tap handler for single-OK mode**

In the tap handler for approval buttons (around line 1128-1149), add the single-OK case:

```cpp
    if (inPrompt) {
      if (ty >= H - 84) {
#if ASK_USER_MODE == 1
        bool approve = isAskUserQuestion() ? true : (tx < W / 2);
#else
        bool approve = tx < W / 2;
#endif
        char cmd[96];
        snprintf(cmd, sizeof(cmd),
                 "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}",
                 tama.promptId, approve ? "once" : "deny");
        sendCmd(cmd);
        responseSent = true;
        if (approve) {
          beep(2400, 60);
          uint32_t tookS = (millis() - promptArrivedMs) / 1000;
          statsOnApproval(tookS);
          if (tookS < 5) triggerOneShot(P_HEART, 2000);
        } else {
          beep(600, 60);
          statsOnDenial();
        }
      }
    }
```

- [ ] **Step 6: Build and flash with mode 0 (auto-approve)**

Set `#define ASK_USER_MODE 0`, build and flash:

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run -t upload
```

Test: trigger an AskUserQuestion from Claude Desktop. Observe:
- Does the device auto-approve without user interaction?
- On the desktop, does the question still appear for the user to answer?
- Or does Claude receive an empty response and continue?

Record the result.

- [ ] **Step 7: Test mode 1 (single OK)**

Set `#define ASK_USER_MODE 1`, build and flash. Test same scenario. Record result.

- [ ] **Step 8: Test mode 2 (better label)**

Set `#define ASK_USER_MODE 2`, build and flash. Test same scenario. Record result.

- [ ] **Step 9: Pick winner and remove the #define switch**

Based on test results, hardcode the winning behavior and remove the `#define ASK_USER_MODE` and the `#if` branches for the losing modes. Clean up to simple `if (isAskUserQuestion())` checks.

- [ ] **Step 10: Build final version**

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run
```

- [ ] **Step 11: Commit**

```powershell
git add src/main.cpp
git commit -m "feat: special-case AskUserQuestion on approval screen

[describe winning behavior based on test results]"
```

---

### Task 6: Final integration test

**Files:** None (testing only)

- [ ] **Step 1: Flash and connect to Claude Desktop**

```powershell
C:\Users\tsphan-ahc\.platformio\penv\Scripts\pio.exe run -t upload
```

Connect over BLE. Verify all features together:

- [ ] **Step 2: Verify home screen**

- Token counter visible right-aligned on bottom bar
- Two transcript lines visible below session counts
- Buddy renders at full size (not cut off by 80px bottom bar)
- Token counter formats correctly: under 1K shows raw number, 1K-999K shows `NNN.NK`, 1M+ shows `N.NM`

- [ ] **Step 3: Verify transcript page**

- Tap from home reaches transcript page (not pet page)
- All available transcript lines displayed with left border
- Newest entry at top, colors fade from bright to dim
- Word-wrap works for long entries
- Drag scrolls if content overflows
- Scrollbar appears when scrollable
- New data resets scroll position

- [ ] **Step 4: Verify tap cycle**

- home -> transcript -> pet -> info -> home (four taps for full cycle)
- All existing screens still render correctly
- Status bars appear on all screens

- [ ] **Step 5: Verify approval screen**

- Normal tool prompts still show OK/NO with full tool name
- Prompt hint text no longer truncated (256 char buffer)
- AskUserQuestion shows chosen behavior from Task 5

- [ ] **Step 6: Verify protocol constraint**

- Open Hardware Buddy window on desktop
- Stats panel shows "Name", battery %, uptime, etc. -- no "No response" error
- Confirm `bat.mA: 0` is still present in status ack (unchanged in xfer.h)
