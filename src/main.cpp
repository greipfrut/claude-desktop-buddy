#include <Arduino.h>
#include <TFT_eSPI.h>
#include <FS.h>
#include <LittleFS.h>
#include <stdarg.h>
using fs::File;

#include "display.h"
#include "touch.h"
#include "clock.h"
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"
#include "character.h"
#include "stats.h"

// ───────────────────────────────────────────────────────────────────────────
// Sprite + panel geometry
// ───────────────────────────────────────────────────────────────────────────
// Full-panel sprite — 240×320 pixels maps 1:1 to the ST7789 panel. Species
// buddies center horizontally via BUDDY_X_CENTER=120; menus, info pages
// and HUD all use W/H so they re-center themselves. The sprite buffer is
// 150 KB, fits in internal SRAM alongside BLE/TFT/LittleFS allocations.
const int W = LCD_WIDTH;    // 240
const int H = LCD_HEIGHT;   // 320
const int CX = W / 2;
const int SPR_X = 0;
const int SPR_Y = 0;

TFT_eSPI    tft;
TFT_eSprite spr = TFT_eSprite(&tft);

// ───────────────────────────────────────────────────────────────────────────
// Battery + power helpers (Waveshare S3 Touch 2.8)
// ───────────────────────────────────────────────────────────────────────────
// The board has no AXP192 — battery voltage comes from an ADC divider on
// GPIO8 (reference: Waveshare demo BAT_Driver.cpp), and "USB present" is
// inferred by voltage above the nominal Li-Po range. Current is unknown,
// so the status responder reports 0. Values are read lazily and cached
// for 1s to keep I/O off the render loop.
static constexpr int PIN_BAT_ADC = 8;
static uint32_t _batLastReadMs = 0;
static int      _batMV         = 0;

static void batteryRead() {
  uint32_t now = millis();
  if (_batMV && now - _batLastReadMs < 1000) return;
  _batLastReadMs = now;
  uint32_t raw = analogReadMilliVolts(PIN_BAT_ADC);
  _batMV = (int)(raw * 3);    // external 1/3 divider on the demo reference
}

int batteryMilliVolts() { batteryRead(); return _batMV; }

int batteryPercent() {
  int mV = batteryMilliVolts();
  int pct = (mV - 3200) / 10;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

int batteryMilliAmps() { return 0; }   // no shunt → unknown

// USB-present heuristic: Li-Po tops out ~4.2V; anything higher means the
// charger is pushing it (USB in). Same threshold the original used.
bool batteryUsbPresent() { return batteryMilliVolts() > 4250; }
int  batteryUsbMilliVolts() { return batteryUsbPresent() ? 5000 : 0; }

// Soft-power latch — GPIO7 held HIGH by PWR_Init() during boot. Releasing
// it drops the rail, mimicking M5.Axp.PowerOff() while on battery. On USB
// the pin drop is harmless (the rail stays up through the USB 5V → LDO),
// so "turn off" behaves like screen-off instead.
static void powerOff() {
  ledcWrite(BL_PWM_CHANNEL, 0);
  digitalWrite(PWR_Control_PIN, LOW);
  delay(500);
  ESP.restart();   // on USB the latch doesn't cut power; reboot is the fallback
}

// ───────────────────────────────────────────────────────────────────────────
// BLE advertise name: "Claude-XXXX" from last two BT MAC bytes
// ───────────────────────────────────────────────────────────────────────────
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

// ───────────────────────────────────────────────────────────────────────────
// Push sprite → panel via byte-swap + LCD_addWindow
// ───────────────────────────────────────────────────────────────────────────
// TFT_eSprite stores 16-bit pixels in native (little-endian) order; the
// ST7789 expects MSB-first. Swap in place, send, swap back so the buffer
// stays consistent for the next drawing pass.
static void blit() {
  uint16_t* buf = (uint16_t*)spr.getPointer();
  uint32_t n = (uint32_t)W * H;
  for (uint32_t i = 0; i < n; i++) buf[i] = __builtin_bswap16(buf[i]);
  LCD_addWindow(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, buf);
  for (uint32_t i = 0; i < n; i++) buf[i] = __builtin_bswap16(buf[i]);
}

// Colors shared across UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background
const uint16_t GREEN = 0x07E0;
const uint16_t RED   = 0xF800;

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 2;                  // 0..4 → 20..100%

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;

static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}

uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;
uint32_t promptArrivedMs = 0;

static void applyBrightness() {
  static const uint8_t LVLS[] = { 20, 40, 60, 80, 100 };
  Set_Backlight(LVLS[brightLevel]);
}

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}

bool responseSent = false;

// No buzzer on this board — beep() is a stub so call sites don't branch.
static void beep(uint16_t, uint16_t) {}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  spr.fillSprite(0x0000);
  characterInvalidate();
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "bright", "sound", "bt", "wifi", "led", "hud", "pet", "reset", "back" };
const uint8_t SETTINGS_N = 9;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "del char", "factory", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2: s.bt = !s.bt; break;
    case 3: s.wifi = !s.wifi; break;
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: nextPet(); return;
    case 7: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 8: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    return;
  }

  if (idx == 0) {
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

const int MENU_HINT_H = 22;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 6, mw - 12, p.textDim);
  spr.setTextSize(2);
  spr.setTextColor(p.textDim, PANEL);
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 12 + 6;
  spr.fillTriangle(x, hy + 2, x + 10, hy + 2, x + 5, hy + 12, p.textDim);
  x = mx + mw / 2 + 6;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 12 + 6;
  spr.fillTriangle(x, hy, x, hy + 12, x + 8, hy + 6, p.textDim);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = 236, mh = 16 + SETTINGS_N * 28 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 8, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 8, p.textDim);
  spr.setTextSize(3);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 12 + i * 28);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setTextSize(2);
    spr.setCursor(mx + mw - 64, my + 16 + i * 28);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
    spr.setTextSize(3);
  }
  drawMenuHints(p, mx, mw, my + mh - 18, "Next", "Change");
  spr.setTextSize(1);
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 236, mh = 20 + RESET_N * 36 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 8, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 8, HOT);
  spr.setTextSize(3);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 8, my + 16 + i * 36);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 18);
  spr.setTextSize(1);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: powerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 236, mh = 20 + MENU_N * 36 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 8, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 8, p.textDim);
  spr.setTextSize(3);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 8, my + 16 + i * 36);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? " on" : " off");
  }
  drawMenuHints(p, mx, mw, my + mh - 18);
  spr.setTextSize(1);
}

// ───────────────────────────────────────────────────────────────────────────
// Clock (portrait only — no IMU, so landscape auto-rotate is dropped)
// ───────────────────────────────────────────────────────────────────────────
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = batteryUsbPresent();
  clockGetTime(&_clkTm);
  clockGetDate(&_clkDt);
}

static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkDt.WeekDay % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.Seconds);
  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;
  char dl[12]; snprintf(dl, sizeof(dl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkDt.Date);

  // Pet peek occupies y<100; clock owns 110..H.
  spr.fillRect(0, 110, W, H - 110, p.bg);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(7); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 170);
  spr.setTextSize(4); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 230);
  spr.setTextSize(3);                                     spr.drawString(dl, CX, 280);
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextSize(2);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(6, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 56, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 22;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(6, y); spr.print(section);
  y += 22;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(3);
  spr.setTextColor(p.textDim, p.bg);
  // "BT PAIRING" = 10 chars × 18 = 180 px, centered
  spr.setCursor((W - 10 * 18) / 2, 60); spr.print("BT PAIRING");
  spr.setCursor((W - 17 * 12) / 2, H - 70);
  spr.setTextSize(2);
  spr.print("enter on desktop:");
  spr.setTextSize(6);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 6 * 36) / 2, (H - 48) / 2);
  spr.print(b);
  spr.setTextSize(1);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 110;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(2);
  int y = TOP + 2;
  // Line-height 18 for size-2 (16px glyph + 2px leading).
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(6, y); spr.print(b); y += 18;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your");
    ln("Claude sessions.");
    y += 6;
    ln("Tap bottom to");
    spr.setTextColor(p.text, p.bg);
    ln("approve prompts.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("Settings > pet");
    ln("to cycle species.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "TOUCH", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("tap bottom");
    spr.setTextColor(p.textDim, p.bg); ln(" next / approve");
    y += 4;
    spr.setTextColor(p.text, p.bg);    ln("tap top");
    spr.setTextColor(p.textDim, p.bg); ln(" page / deny");
    y += 4;
    spr.setTextColor(p.text, p.bg);    ln("long press");
    spr.setTextColor(p.textDim, p.bg); ln(" menu on/off");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln(" sessions %u", tama.sessionsTotal);
    ln(" running  %u", tama.sessionsRunning);
    ln(" waiting  %u", tama.sessionsWaiting);
    y += 6;
    spr.setTextColor(p.text, p.bg);   ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln(" age  %lus", (unsigned long)age);
    ln(" %s", !bleConnected() ? "idle" : bleSecure() ? "secure" : "OPEN");

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = batteryMilliVolts();
    int pct = batteryPercent();
    bool usb = batteryUsbPresent();
    bool charging = usb && vBat_mV < 4100;
    bool full = usb && vBat_mV >= 4100;

    spr.setTextSize(3);
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(6, y); spr.printf("%d%%", pct);
    spr.setTextSize(2);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(100, y + 6);
    spr.print(full ? "full" : (charging ? "chrg" : (usb ? "usb" : "bat")));
    y += 28;

    spr.setTextColor(p.textDim, p.bg);
    ln(" %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    uint32_t up = millis() / 1000;
    ln(" up %luh%02lum", up / 3600, (up / 60) % 60);
    ln(" heap %uK", ESP.getFreeHeap() / 1024);
    ln(" bt %s", settings().bt ? (dataBtActive() ? "link" : "on") : "off");

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextSize(3);
    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setCursor(6, y);
    spr.print(linked ? "linked" : (settings().bt ? "pair" : "off"));
    spr.setTextSize(2);
    y += 28;

    spr.setTextColor(p.text, p.bg);    ln(" %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln(" %lus ago", (unsigned long)age);
    } else if (settings().bt) {
      ln(" Open Claude");
      ln(" desktop app");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg); ln("made by");
    spr.setTextColor(p.text, p.bg);    ln("Felix Rieseberg");
    y += 6;
    spr.setTextColor(p.textDim, p.bg); ln("hardware");
    spr.setTextColor(p.text, p.bg);    ln("Waveshare S3");
    ln("Touch LCD 2.8");
  }
  spr.setTextSize(1);
}

static uint8_t wrapInto(const char* in, char out[][40], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

static void drawApproval() {
  const Palette& p = characterPalette();
  const int AREA = 140;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  spr.setTextSize(2);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, H - AREA + 6);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 13 ? 3 : 2);
  spr.setCursor(6, H - AREA + 28);
  spr.print(tama.promptTool);

  spr.setTextSize(2);
  spr.setTextColor(p.textDim, p.bg);
  // At size 2 the panel fits ~19 chars with 6px margin; wrap longer hints.
  char line[20];
  int hlen = strlen(tama.promptHint);
  int shown = hlen > 19 ? 19 : hlen;
  memcpy(line, tama.promptHint, shown); line[shown] = 0;
  spr.setCursor(6, H - AREA + 68);
  spr.print(line);
  if (hlen > 19) {
    int left = hlen - 19; if (left > 19) left = 19;
    memcpy(line, tama.promptHint + 19, left); line[left] = 0;
    spr.setCursor(6, H - AREA + 88);
    spr.print(line);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(6, H - 22);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(6, H - 22);
    spr.print("v approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 86, H - 22);
    spr.print("^ deny");
  }
  spr.setTextSize(1);
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 4, y, 4, col);
    spr.fillCircle(x + 4, y, 4, col);
    spr.fillTriangle(x - 8, y + 2, x + 8, y + 2, x, y + 10, col);
  } else {
    spr.drawCircle(x - 4, y, 4, col);
    spr.drawCircle(x + 4, y, 4, col);
    spr.drawLine(x - 8, y + 2, x, y + 10, col);
    spr.drawLine(x + 8, y + 2, x, y + 10, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = 110;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(2);
  int y = TOP + 28;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 4); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(100 + i * 24, y + 4, i < mood, moodCol);

  y += 28;
  spr.setCursor(6, y - 4); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 80 + i * 14;
    if (i < fed) spr.fillCircle(px, y + 2, 4, p.body);
    else spr.drawCircle(px, y + 2, 4, p.textDim);
  }

  y += 28;
  spr.setCursor(6, y - 4); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 100 + i * 22;
    if (i < en) spr.fillRect(px, y - 4, 16, 12, enCol);
    else spr.drawRect(px, y - 4, 16, 12, p.textDim);
  }

  y += 30;
  spr.fillRoundRect(6, y - 4, 72, 22, 4, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(14, y); spr.printf("Lv %u", stats().level);

  y += 28;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);
  spr.printf("ok  %u", stats().approvals);
  spr.setCursor(120, y);
  spr.printf("no %u", stats().denials);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(6, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tok ",   stats().tokens,      y + 20);
  tokFmt("day ",   tama.tokensToday,    y + 40);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 110;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(2);
  int y = TOP + 16;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += 18;
  };
  auto gap = [&]() { y += 6; };

  ln(p.body,    "MOOD");
  ln(p.textDim, " fast ok = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tok = lvl");  gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " drains slowly"); gap();

  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "long press: menu");
  spr.setTextSize(1);
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 110;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  spr.setTextSize(2);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(6, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 56, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
  spr.setTextSize(1);
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  // At size 2 the panel fits 20 chars (240/12); pad to 19 for the margin.
  // 5 lines × 18 px line-height = 90, plus 4 px padding.
  const int SHOW = 5, LH = 18, WIDTH = 19;
  const int AREA = SHOW * LH + 4;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.setTextSize(2);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(6, H - LH - 2);
    spr.print(tama.msg);
    spr.setTextSize(1);
    return;
  }

  static char disp[48][40];
  static uint8_t srcOf[48];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 48; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 48 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(6, H - AREA + 2 + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 36, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
  spr.setTextSize(1);
}

// ───────────────────────────────────────────────────────────────────────────
// Touch gesture decoder
// ───────────────────────────────────────────────────────────────────────────
// Physical buttons map to screen zones in panel coordinates (0..239×0..319):
//   top band  (y <  110) → "B": next page / deny / confirm
//   bot band  (y >= 210) → "A": next screen / approve / menu next-item
//   middle    (else)     → used for long-press menu open/close
//
// Short tap = released within 500ms without moving much.
// Long press = held > 600ms (fires once, suppresses short-tap).
enum GestureKind { G_NONE, G_TAP_A, G_TAP_B, G_LONG };

static uint32_t gDownMs       = 0;
static uint32_t gLastSeenMs   = 0;
static bool     gHeld         = false;
static bool     gLongFired    = false;
static uint16_t gDownX        = 0;
static uint16_t gDownY        = 0;

static GestureKind pollGesture() {
  uint16_t x, y;
  bool have = touchGetPoint(&x, &y);
  uint32_t now = millis();

  if (have) {
    if (!gHeld) {
      gHeld = true;
      gDownMs = now;
      gLongFired = false;
      gDownX = x; gDownY = y;
    }
    gLastSeenMs = now;
  }

  if (gHeld && !gLongFired && now - gDownMs > 600) {
    gLongFired = true;
    return G_LONG;
  }

  if (gHeld && !have && now - gLastSeenMs > 100) {
    gHeld = false;
    if (!gLongFired && now - gDownMs < 500) {
      return (gDownY >= 210) ? G_TAP_A : (gDownY < 110 ? G_TAP_B : G_TAP_A);
    }
  }
  return G_NONE;
}

void setup() {
  Serial.begin(115200);

  PWR_Init();
  Backlight_Init();
  LCD_Init();

  spr.setColorDepth(16);
  if (!spr.createSprite(W, H)) {
    Serial.println("sprite alloc failed");
  }
  spr.fillSprite(TFT_BLACK);
  blit();   // paint the full panel black once before first content

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");

  startBt();
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();

  characterInit(nullptr);
  gifAvailable = characterLoaded();
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  bool touchOk = touchInit();
  Serial.printf("touch init: %s\n", touchOk ? "ok" : "failed");

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(4);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 24);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 24);
    } else {
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 24);
      spr.setTextSize(2);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 20);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    blit();
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // Prompt arrival: beep (no-op), jump to normal, clear overlays.
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // ── Touch dispatch ────────────────────────────────────────────────────
  GestureKind g = pollGesture();

  // Any touch wakes the screen. In screen-off the first gesture is
  // consumed by the wake — no secondary action fires.
  if (g != G_NONE) {
    if (screenOff) {
      wake();
      g = G_NONE;   // swallow
    } else {
      wake();
    }
  }

  if (g == G_LONG) {
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
  } else if (g == G_TAP_A) {
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      uint32_t tookS = (millis() - promptArrivedMs) / 1000;
      statsOnApproval(tookS);
      if (tookS < 5) triggerOneShot(P_HEART, 2000);
    } else if (resetOpen) {
      resetSel = (resetSel + 1) % RESET_N;
      resetConfirmIdx = 0xFF;
    } else if (settingsOpen) {
      settingsSel = (settingsSel + 1) % SETTINGS_N;
    } else if (menuOpen) {
      menuSel = (menuSel + 1) % MENU_N;
    } else {
      displayMode = (displayMode + 1) % DISP_COUNT;
      applyDisplayMode();
    }
  } else if (g == G_TAP_B) {
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
    } else if (resetOpen) {
      applyReset(resetSel);
    } else if (settingsOpen) {
      applySetting(settingsSel);
    } else if (menuOpen) {
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else {
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  // ── Clock (portrait only; no IMU → no auto-rotate) ────────────────────
  clockRefreshRtc();
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;

  static bool wasClocking = false;
  if (clocking != wasClocking) {
    if (clocking) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);
    uint8_t h = _clkTm.Hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) wake();
  lastPasskey = pk;

  if (screenOff) {
    // skip render entirely
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(2);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(12, 140); spr.print("installing");
      spr.setCursor(12, 164); spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 24;
      spr.drawRect(12, 196, barW, 14, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 2) spr.fillRect(14, 198, fill - 2, 10, p.body);
      }
    } else {
      spr.setCursor(12, 150);
      spr.print("no character");
    }
    spr.setTextSize(1);
  }

  if (!screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (settings().hud) drawHUD();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
  }

  // ~30fps blit — slower frames keep transcript-heavy renders stable.
  static uint32_t lastBlit = 0;
  if (!screenOff && millis() - lastBlit >= 33) {
    lastBlit = millis();
    blit();
  }

  // Auto screen-off on battery only — while charging we leave the clock up.
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    ledcWrite(BL_PWM_CHANNEL, 0);
    screenOff = true;
  }

  delay(screenOff ? 100 : 8);
}
