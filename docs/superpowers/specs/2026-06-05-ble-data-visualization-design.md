# BLE Data Visualization Design

Use the 480x480 display to show more of what Claude Desktop sends over BLE.

## Background

The device receives rich data every heartbeat (session counts, transcript
entries, token counts, permission prompts) and on turn events, but most of
it is truncated or invisible. Buffers are too small for real content, the
transcript feed was dropped during the 480x480 rewrite, tokens_today is
buried in a sub-page, AskUserQuestion shows misleading OK/NO buttons, and
turn events are completely ignored.

## Changes

### 1. Enlarge data buffers (data.h)

Current sizes truncate real data:

| Buffer | Current | New | Rationale |
|---|---|---|---|
| `msg` | 24 chars | 100 chars | Status messages like "(editing src/main.cpp...)" are 40-80 chars |
| `promptHint` | 44 chars | 256 chars | Tool hints can be full command lines; approval screen already word-wraps |
| `promptTool` | 20 chars | 48 chars | Tool names like "AskUserQuestion" are 15+ chars |

RAM cost: ~340 bytes (from ~88 to ~428 for these three fields). Negligible
on ESP32-S3 with 8MB PSRAM.

Update `strncpy` bounds in `_applyJson()` to use `sizeof()` instead of
hardcoded sizes.

### 2. Home bottom bar: transcript preview + token counter

Grow the bottom bar from 40px to ~80px. New layout:

```
Line 1: "2 running  1 waiting  lv 5"  [left]   "361K" [right, accent]
Line 2: "14:32 editing src/main.cpp..."          [dim, newest entry]
Line 3: "14:31 running tests..."                 [dimmer, second entry]
```

- Token counter (`tokens_today`) right-aligned on the first line, formatted
  as `361K` or `1.2M`, using the palette's accent/body color.
- Two most recent `tama.lines[]` entries below, newest first.
- `tama.msg` is no longer displayed separately -- `entries[0]` provides the
  same or more informative content.

The buddy area shrinks from ~400px to ~360px of vertical space. ASCII
buddies at 3x are ~168px and the largest GIF characters are ~200px, so
360px is still generous.

### 3. Dedicated transcript page (DISP_TRANSCRIPT)

New display mode inserted into the tap cycle:

```
home -> transcript -> pet -> info -> home
```

The enum becomes:
```cpp
enum DisplayMode { DISP_NORMAL, DISP_TRANSCRIPT, DISP_PET, DISP_INFO, DISP_COUNT };
```

Transcript page layout (480x480):
- **Top bar (y 0..40):** shared drawStatusBars() top section (clock, name,
  battery) -- same as all other screens.
- **Summary line (y 40..60):** session counts + tokens_today, same format as
  the home bottom bar's first line.
- **Transcript body (y 60..440):** all `tama.lines[]` entries (up to 8),
  rendered with:
  - Left border accent (2px vertical line per entry, palette body color)
  - 8px left padding after border
  - Text size 2 (12x16 px per char at this size)
  - Word-wrap at 37 chars per line (480 - 28px margins = 452px / 12px)
  - Entries that wrap get proportionally more vertical space
  - Newest entry at top, dimmer color for older entries (gradient from
    palette text -> textDim)
  - Scrollable via drag gesture if total wrapped height exceeds 380px
    (reuse the approval screen's drag pattern: `G_DRAG` sets a
    `transcriptScroll` offset, clamped to content bounds)
- **Bottom bar (y 440..480):** "tap: home  hold: menu" hint text, centered,
  dim color.

State: `int transcriptScroll = 0` in main.cpp globals, alongside the
existing `hintScroll`. When `tama.lineGen` changes (new transcript data),
reset `transcriptScroll` to 0 (top/newest) -- same pattern as the existing
`msgScroll`/`lastLineGen` reset in `drawHUD()`. Drag handling: in the
main loop's `G_DRAG` branch, when `displayMode == DISP_TRANSCRIPT`,
apply `transcriptScroll -= gDragDy` (same sign convention as approval
hint scroll).

### 4. AskUserQuestion handling (empirical)

The correct behavior when `prompt.tool == "AskUserQuestion"` is unclear
without testing against real Claude Desktop behavior. Three candidates:

- **Auto-approve:** automatically send `{"cmd":"permission","id":"...","decision":"once"}`
  and show "question pending -- answer on desktop" on the approval screen.
- **Single OK button:** show info text + one OK button (no NO). User must
  manually approve.
- **Better label:** keep OK/NO but change header from the tool name to
  "question for you -- answer on desktop".

Implementation plan: build all three behind a simple switch, test each
against a real Claude Desktop session, observe whether approving causes
Claude to receive an empty response and continue or correctly waits for
the desktop answer. Pick the winner based on observed behavior. The
switch can be a `#define` or a settings toggle if multiple behaviors are
valid depending on context.

### 5. evt:turn parser (data.h)

Parse turn events:
```json
{"evt": "turn", "role": "assistant", "content": [{"type": "text", "text": "..."}]}
```

Add to TamaState:
```cpp
char lastTurnRole[12];       // "assistant" or "user"
char lastTurnSnippet[120];   // first text block, truncated
uint32_t lastTurnMs;         // millis() when received
```

Parser logic in `_applyJson()`:
- Check for `doc["evt"]` string equal to `"turn"`.
- Copy `doc["role"]` into `lastTurnRole`.
- Iterate `doc["content"]` array, find first object with `type == "text"`,
  copy its `text` field (truncated to 119 chars) into `lastTurnSnippet`.
- Ignore content blocks that aren't `type: "text"` (tool_use, tool_result).
- Events >4KB are dropped by the desktop before sending, so no overflow
  concern.

No UI display in this round. The stored data will be available for a
future transcript page enhancement or home screen "last said" indicator.

## Files modified

| File | Changes |
|---|---|
| `src/data.h` | Buffer sizes, TamaState fields for turn events, `_applyJson()` parser additions |
| `src/main.cpp` | `DisplayMode` enum, `drawStatusBars()` expanded bottom bar, new `drawTranscript()`, tap cycle routing, AskUserQuestion branch in `drawApproval()`, `transcriptScroll` state + drag handling |

## Not in scope

- Full evt:turn UI display (deferred to after transcript page exists)
- Multi-choice prompt rendering (needs desktop-bridge protocol changes)
- Token usage visualization / bar charts (future enhancement)
- 480x480 layout changes to buddy scaling or top bar

## Protocol constraint

The desktop's status ack JSON parsing is strict. All fields in the status
response must be present, including `bat.mA: 0`. Removing any field causes
"No response" in the Hardware Buddy stats panel. This design does not
change the status ack format.
