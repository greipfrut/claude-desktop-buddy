#include <Arduino.h>
#include <TFT_eSPI.h>
#include <LittleFS.h>

#include "display.h"
#include "touch.h"
#include "buddy.h"
#include "character.h"

// 135x240 preserves the original M5StickC sprite coordinates used by every
// src/buddies/*.cpp file. Centered inside the 240x320 panel.
static constexpr int SPR_W = 135;
static constexpr int SPR_H = 240;
static constexpr int SPR_X = (LCD_WIDTH  - SPR_W) / 2;   // 52
static constexpr int SPR_Y = (LCD_HEIGHT - SPR_H) / 2;   // 40

// TFT_eSPI is only used here for its TFT_eSprite buffer/drawing API —
// we never call tft.init(), the demo's LCD_Init() drives the panel.
TFT_eSPI tft;
TFT_eSprite spr = TFT_eSprite(&tft);

static uint8_t activeState = 1;     // PersonaState idle
static bool    useAscii    = true;
static uint32_t touchDownMs = 0;
static bool     touchHeld   = false;

static void blit() {
  // TFT_eSprite stores 16-bit pixels in native (little-endian) order; the
  // ST7789 expects MSB-first. Swap in place, send, swap back so the
  // sprite buffer stays consistent for the next drawing pass.
  uint16_t* buf = (uint16_t*)spr.getPointer();
  uint32_t n = (uint32_t)SPR_W * SPR_H;
  for (uint32_t i = 0; i < n; i++) buf[i] = __builtin_bswap16(buf[i]);
  LCD_addWindow(SPR_X, SPR_Y, SPR_X + SPR_W - 1, SPR_Y + SPR_H - 1, buf);
  for (uint32_t i = 0; i < n; i++) buf[i] = __builtin_bswap16(buf[i]);
}

static void onShortTap(uint16_t x, uint16_t y) {
  // Bottom third: cycle species. Else: cycle emotion state.
  if (y > 220) {
    if (useAscii) buddyNextSpecies();
    buddyInvalidate();
    characterInvalidate();
  } else {
    activeState = (activeState + 1) % 7;
    buddyInvalidate();
    characterSetState(activeState);
  }
}

static void onLongPress() {
  if (characterLoaded()) {
    useAscii = !useAscii;
    buddyInvalidate();
    characterInvalidate();
  }
}

static void pollTouch() {
  uint16_t x, y;
  bool have = touchGetPoint(&x, &y);
  uint32_t now = millis();
  if (have) {
    if (!touchHeld) {
      touchHeld   = true;
      touchDownMs = now;
      onShortTap(x, y);
    } else if (now - touchDownMs > 800) {
      onLongPress();
      touchDownMs = now + 10000;
    }
  } else if (touchHeld && now - touchDownMs > 120) {
    touchHeld = false;
  }
}

void setup() {
  Serial.begin(115200);

  PWR_Init();
  Backlight_Init();
  LCD_Init();

  spr.setColorDepth(16);
  spr.createSprite(SPR_W, SPR_H);
  spr.fillSprite(TFT_BLACK);

  // Boot diagnostic: paint fullscreen R/G/B via the demo path — values are
  // pre-swapped to MSB-first because ST7789 wants bytes in that order.
  {
    const uint16_t W = LCD_WIDTH, H = LCD_HEIGHT;
    uint16_t* band = (uint16_t*)malloc(W * 32 * sizeof(uint16_t));
    auto flood = [&](uint16_t color) {
      uint16_t c = __builtin_bswap16(color);
      for (int i = 0; i < W * 32; i++) band[i] = c;
      for (int y = 0; y < H; y += 32) {
        uint16_t yend = y + 31; if (yend >= H) yend = H - 1;
        LCD_addWindow(0, y, W - 1, yend, band);
      }
    };
    flood(0xF800); delay(350);
    flood(0x07E0); delay(350);
    flood(0x001F); delay(350);
    flood(0x0000);
    free(band);
  }

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");

  buddyInit();
  characterInit(nullptr);
  useAscii = !characterLoaded();

  bool touchOk = touchInit();
  Serial.printf("touch init: %s\n", touchOk ? "ok" : "failed");

  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setTextSize(2);
  spr.drawString("Hello!", SPR_W / 2, SPR_H / 2);
  blit();
  delay(800);

  characterSetState(activeState);
}

void loop() {
  pollTouch();
  // buddyTick / characterTick clear and repaint their own region only when
  // the animation frame advances or state changes — never wipe the sprite
  // here, and only blit at ~30fps to let slow frames stay stable.
  if (useAscii) buddyTick(activeState);
  else          characterTick();

  static uint32_t lastBlit = 0;
  uint32_t now = millis();
  if (now - lastBlit >= 33) {
    lastBlit = now;
    blit();
  }
}
