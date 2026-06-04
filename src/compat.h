#pragma once
// Compatibility shims for TFT_eSPI -> Arduino_GFX migration.
// Keeps diffs minimal across the 20+ files that reference TFT_eSPI types.

#include <Arduino_GFX_Library.h>

// ── Color constant aliases ─────────────────────────────────────────────
// TFT_eSPI uses TFT_BLACK etc. Arduino_GFX uses RGB565_BLACK etc.
// Map both the TFT_* names and the bare names used in the codebase.
#ifndef BLACK
#define BLACK       0x0000
#endif
#ifndef WHITE
#define WHITE       0xFFFF
#endif
#ifndef RED
#define RED         0xF800
#endif
#ifndef GREEN
#define GREEN       0x07E0
#endif
#ifndef BLUE
#define BLUE        0x001F
#endif
#ifndef TFT_BLACK
#define TFT_BLACK   BLACK
#endif
#ifndef TFT_WHITE
#define TFT_WHITE   WHITE
#endif
#ifndef TFT_RED
#define TFT_RED     RED
#endif
#ifndef TFT_GREEN
#define TFT_GREEN   GREEN
#endif
#ifndef TFT_BLUE
#define TFT_BLUE    BLUE
#endif

// ── Text datum constants ───────────────────────────────────────────────
// TFT_eSPI text alignment values — used in setTextDatum() calls that we
// replace with drawCenteredString(). Define them so any straggler compiles.
#ifndef MC_DATUM
#define MC_DATUM 4
#endif
#ifndef TL_DATUM
#define TL_DATUM 0
#endif

// ── Centered text helper ───────────────────────────────────────────────
// Replaces setTextDatum(MC_DATUM) + drawString(). Arduino_GFX does not
// have text alignment control, so we compute it manually.
inline void drawCenteredString(Arduino_GFX* gfx, const char* s,
                               int16_t cx, int16_t cy) {
  int16_t x1, y1;
  uint16_t tw, th;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  gfx->setCursor(cx - tw / 2, cy - th / 2);
  gfx->print(s);
}
