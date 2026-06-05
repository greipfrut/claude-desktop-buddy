#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_mac.h>
#include <FS.h>
#include <LittleFS.h>
#include <stdarg.h>
using fs::File;

#include "compat.h"
#include "display.h"
#include "touch.h"
#include "clock.h"
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"
#include "character.h"
#include "stats.h"
#include "audio.h"
#include "power.h"

// Back the audio module's settings accessors (declared in audio.h) with the
// real Settings instance. audio.cpp can't include stats.h (file-static state),
// so it reads volume/waveform through these from the single owning TU.
uint8_t audioVolume()   { return settings().volume; }
bool    audioSineWave() { return settings().sineWave; }

// ───────────────────────────────────────────────────────────────────────────
// Canvas + panel geometry
// ───────────────────────────────────────────────────────────────────────────
// The global `canvas` (Arduino_Canvas*) is created by displayInit() in
// display.cpp. The macro below lets every existing `spr.method()` call
// compile as `(*canvas).method()` without touching each call site.
#define spr (*canvas)

const int W = LCD_WIDTH;
const int H = LCD_HEIGHT;
const int CX = W / 2;

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
// Push canvas → RGB panel via DMA flush
// ───────────────────────────────────────────────────────────────────────────
static void blit() {
  canvas->flush();
}

// Colors shared across UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background
// GREEN (0x07E0) and RED (0xF800) are defined as macros in compat.h

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

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
int      hintScroll = 0;
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
  Set_Backlight(LVLS[settings().bright]);
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
static bool lastBtSetting = true;


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
  spr.fillScreen(0x0000);
  characterInvalidate();
}

// Force a full-screen redraw after closing an overlay. characterInvalidate()
// is a no-op when no gif character is loaded (e.g. buddy mode), so the
// overlay's pixels would otherwise persist in the sprite buffer.
static void redrawAll() {
  spr.fillScreen(characterPalette().bg);
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "bright", "sound", "gesture", "bt", "pair", "led", "hud", "pet", "reset", "back" };
const uint8_t SETTINGS_N = 10;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "del char", "factory", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

bool    audioOpen = false;
uint8_t audioSel  = 0;
const char* audioItems[] = { "volume", "wave", "back" };
const uint8_t AUDIO_N = 3;

static bool pairTrigger = false;

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

static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; redrawAll(); return; }

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

const int MENU_HINT_H = 18;
static const int APPROVE_BTN_H = 56;

static void drawMenuHints(const Palette& p, int mx, int mw, int hy) {
  spr.drawFastHLine(mx + 6, hy - 6, mw - 12, p.textDim);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, PANEL);
  const char* hint = "hold to close";
  int tw = strlen(hint) * 6;
  spr.setCursor(mx + (mw - tw) / 2, hy);
  spr.print(hint);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = 360, mh = 16 + SETTINGS_N * 36 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 8, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 8, p.textDim);
  spr.setTextSize(3);
  Settings& s = settings();
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 10, my + 14 + i * 36);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setTextSize(2);
    spr.setCursor(mx + mw - 70, my + 18 + i * 36);
    spr.setTextColor(p.textDim, PANEL);
    switch (i) {
      case 0: spr.printf("%u/4", s.bright); break;
      case 1:
        if (s.volume == 0) spr.print("off");
        else               spr.printf("%u/4", s.volume);
        break;
      case 2: spr.setTextColor(s.gestures ? GREEN : p.textDim, PANEL);
              spr.print(s.gestures ? " on" : "off"); break;
      case 3: spr.setTextColor(s.bt ? GREEN : p.textDim, PANEL);
              spr.print(s.bt ? " on" : "off"); break;
      case 4:
        if (!s.bt)                  { spr.print("off"); }
        else if (bleConnected())    { spr.setTextColor(GREEN, PANEL); spr.print(" ok"); }
        else                        { spr.setTextColor(HOT, PANEL);   spr.print(" go"); }
        break;
      case 5: spr.setTextColor(s.led ? GREEN : p.textDim, PANEL);
              spr.print(s.led ? " on" : "off"); break;
      case 6: spr.setTextColor(s.hud ? GREEN : p.textDim, PANEL);
              spr.print(s.hud ? " on" : "off"); break;
      case 7: {
        uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
        uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
        spr.printf("%u/%u", pos, total);
        break;
      }
      default: break;
    }
    spr.setTextSize(3);
  }
  drawMenuHints(p, mx, mw, my + mh - 14);
  spr.setTextSize(1);
}

static void drawAudio() {
  const Palette& p = characterPalette();
  Settings& s = settings();
  int mw = 360, mh = 16 + AUDIO_N * 36 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 8, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 8, p.textDim);
  spr.setTextSize(3);
  for (int i = 0; i < AUDIO_N; i++) {
    bool sel = (i == audioSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 10, my + 14 + i * 36);
    spr.print(sel ? "> " : "  ");
    spr.print(audioItems[i]);
    spr.setTextSize(2);
    spr.setCursor(mx + mw - 70, my + 18 + i * 36);
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

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 360, mh = 20 + RESET_N * 44 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 8, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 8, HOT);
  spr.setTextSize(3);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 12, my + 18 + i * 44);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 14);
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
    case 5: menuOpen = false; redrawAll(); break;
  }
}

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
    if (i == 4) spr.print(dataDemo() ? " on" : " off");
  }
  drawMenuHints(p, mx, mw, my + mh - 14);
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
  spr.setTextSize(9); spr.setTextColor(p.text, p.bg);    drawCenteredString(canvas, hm, CX, 220);
  spr.setTextSize(4); spr.setTextColor(p.textDim, p.bg); drawCenteredString(canvas, ss, CX, 310);
  spr.setTextSize(3);                                     drawCenteredString(canvas, dl, CX, 380);
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
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(10, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 80, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 30;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(10, y); spr.print(section);
  y += 30;
  spr.setTextSize(2);
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillScreen(p.bg);
  spr.setTextSize(4);
  spr.setTextColor(p.textDim, p.bg);
  drawCenteredString(canvas, "BT PAIRING", CX, 80);
  spr.setTextSize(2);
  drawCenteredString(canvas, "enter on desktop:", CX, H - 80);
  spr.setTextSize(8);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  drawCenteredString(canvas, b, CX, H / 2);
  spr.setTextSize(1);
}

static void drawStatusBars();

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 110;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(2);
  int y = TOP + 4;
  auto ln = [&](const char* fmt, ...) {
    char b[40]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(10, y); spr.print(b); y += 24;
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
    spr.setTextColor(p.text, p.bg);    ln("tap screen");
    spr.setTextColor(p.textDim, p.bg); ln(" next page");
    y += 4;
    spr.setTextColor(p.text, p.bg);    ln("tap item");
    spr.setTextColor(p.textDim, p.bg); ln(" pick / confirm");
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
    bool charging = batteryCharging();
    bool full = batteryFull();

    spr.setTextSize(4);
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(10, y); spr.printf("%d%%", pct);
    spr.setTextSize(3);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(140, y + 6);
    spr.print(full ? "full" : (charging ? "chrg" : (usb ? "usb" : "bat")));
    y += 38;

    spr.setTextColor(p.textDim, p.bg);
    ln(" %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    uint32_t up = millis() / 1000;
    ln(" up %luh%02lum", up / 3600, (up / 60) % 60);
    ln(" heap %uK", ESP.getFreeHeap() / 1024);
    ln(" bt %s", !settings().bt ? "off" : (bleConnected() ? (bleSecure() ? "sec" : "open") : "advt"));

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool btOn = settings().bt;
    bool linked = btOn && bleConnected();
    bool sec = linked && bleSecure();
    bool pairing = btOn && blePasskey() != 0;

    spr.setTextSize(3);
    if (!btOn) {
      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(6, y); spr.print("off");
    } else if (pairing) {
      spr.setTextColor(HOT, p.bg);
      spr.setCursor(6, y); spr.print("pairing");
    } else if (sec) {
      spr.setTextColor(GREEN, p.bg);
      spr.setCursor(6, y); spr.print("secure");
    } else if (linked) {
      spr.setTextColor(HOT, p.bg);
      spr.setCursor(6, y); spr.print("open");
    } else {
      spr.setTextColor(p.textDim, p.bg);
      spr.setCursor(6, y); spr.print("ready");
    }
    spr.setTextSize(2);
    y += 28;

    spr.setTextColor(p.text, p.bg); ln(" %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    if (sec) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln(" %lus ago", (unsigned long)age);
    } else if (btOn && !pairing) {
      ln(" Open Claude");
      ln(" desktop app");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg); ln("original");
    spr.setTextColor(p.text, p.bg);    ln("Felix Rieseberg");
    y += 6;
    spr.setTextColor(p.textDim, p.bg); ln("4B port");
    spr.setTextColor(p.text, p.bg);    ln("greipfrut + Claude");
    y += 6;
    spr.setTextColor(p.textDim, p.bg); ln("hardware");
    spr.setTextColor(p.text, p.bg);    ln("Waveshare S3");
    ln("Touch LCD 4B");
  }
  spr.setTextSize(1);
  drawStatusBars();
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
  spr.fillScreen(p.bg);

  // Header: elapsed timer
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  spr.setTextSize(2);
  spr.setTextColor(waited >= 10 ? HOT : p.textDim, p.bg);
  spr.setCursor(12, 12);
  spr.printf("approve? %lus", (unsigned long)waited);

  // Tool name
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(12, 44);
  spr.print(tama.promptTool);

  // Scrollable hint area
  const int HINT_Y = 84, HINT_H = 276, COL_PX = 12, LINE_H = 22;
  const int wrapCols = (W - 28) / COL_PX;
  spr.setTextSize(2);
  spr.setTextColor(p.body, p.bg);

  int n = strlen(tama.promptHint);
  int totalLines = 0;
  for (int i = 0; i < n; i += wrapCols) totalLines++;
  if (totalLines == 0) totalLines = 1;
  int maxScroll = totalLines * LINE_H - HINT_H;
  if (maxScroll < 0) maxScroll = 0;
  if (hintScroll > maxScroll) hintScroll = maxScroll;
  if (hintScroll < 0) hintScroll = 0;

  int yline = HINT_Y - hintScroll;
  char line[64];
  for (int i = 0; i < n; i += wrapCols) {
    int len = (n - i < wrapCols) ? (n - i) : wrapCols;
    if (len > 63) len = 63;
    memcpy(line, tama.promptHint + i, len); line[len] = 0;
    if (yline + LINE_H > HINT_Y && yline < HINT_Y + HINT_H) {
      spr.setCursor(14, yline); spr.print(line);
    }
    yline += LINE_H;
  }

  // Scrollbar (only when content overflows)
  if (maxScroll > 0) {
    int trackH = HINT_H, barH = trackH * HINT_H / (totalLines * LINE_H);
    if (barH < 10) barH = 10;
    int barY = HINT_Y + (trackH - barH) * hintScroll / maxScroll;
    spr.fillRect(W - 6, HINT_Y, 4, trackH, p.bg);
    spr.fillRect(W - 6, barY, 4, barH, p.textDim);
  }

  // OK / NO buttons pinned to bottom
  int btnY = H - 84, btnH = 76;
  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg); spr.setTextSize(3);
    drawCenteredString(canvas, "sent", W / 2, btnY + btnH / 2);
  } else {
    spr.fillRoundRect(8,         btnY, W/2 - 12, btnH, 12, GREEN);
    spr.fillRoundRect(W/2 + 4,   btnY, W/2 - 12, btnH, 12, HOT);
    spr.setTextSize(4);
    spr.setTextColor(0x0000, GREEN); drawCenteredString(canvas, "OK", W/4,     btnY + btnH/2);
    spr.setTextColor(0x0000, HOT);   drawCenteredString(canvas, "NO", 3*W/4,   btnY + btnH/2);
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
  int y = TOP + 32;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(10, y - 4); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(160 + i * 30, y + 4, i < mood, moodCol);

  y += 36;
  spr.setCursor(10, y - 4); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 120 + i * 20;
    if (i < fed) spr.fillCircle(px, y + 2, 5, p.body);
    else spr.drawCircle(px, y + 2, 5, p.textDim);
  }

  y += 36;
  spr.setCursor(10, y - 4); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 160 + i * 28;
    if (i < en) spr.fillRect(px, y - 4, 20, 14, enCol);
    else spr.drawRect(px, y - 4, 20, 14, p.textDim);
  }

  y += 38;
  spr.fillRoundRect(10, y - 4, 80, 26, 4, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(18, y); spr.printf("Lv %u", stats().level);

  y += 34;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(10, y);
  spr.printf("ok  %u", stats().approvals);
  spr.setCursor(180, y);
  spr.printf("no %u", stats().denials);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(10, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tok ",   stats().tokens,      y + 24);
  tokFmt("day ",   tama.tokensToday,    y + 48);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 110;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(2);
  int y = TOP + 20;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(10, y); spr.print(s); y += 24;
  };
  auto gap = [&]() { y += 8; };

  ln(p.body,    "MOOD");
  ln(p.textDim, " fast ok = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tok = lvl");  gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " drains slowly"); gap();

  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "tap = next page");
  ln(p.textDim, "hold = menu");
  spr.setTextSize(1);
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 110;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(10, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 80, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
  spr.setTextSize(1);
  drawStatusBars();
}

static void drawStatusBars() {
  const Palette& p = characterPalette();

  // ── Top bar (y 0..40): clock · pet name · battery ──
  spr.fillRect(0, 0, W, 40, p.bg);
  spr.setTextSize(2);

  if (dataRtcValid()) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(8, 12);
    spr.printf("%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  }

  spr.setTextColor(p.text, p.bg);
  drawCenteredString(canvas, petName(), W / 2, 20);

  int pct = batteryPercent();
  bool chrg = batteryCharging();
  char batBuf[12];
  snprintf(batBuf, sizeof(batBuf), chrg ? "%d%%~" : "%d%%", pct);
  int16_t bx1, by1; uint16_t btw, bth;
  spr.getTextBounds(batBuf, 0, 0, &bx1, &by1, &btw, &bth);
  spr.setTextColor(chrg ? GREEN : p.textDim, p.bg);
  spr.setCursor(W - 8 - btw, 12);
  spr.print(batBuf);

  // ── Bottom bar (y H-40..H): sessions/level · status msg ──
  spr.fillRect(0, H - 40, W, 40, p.bg);
  spr.setTextSize(2);

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, H - 36);
  spr.printf("%u running  %u waiting  lv %u",
    tama.sessionsRunning, tama.sessionsWaiting, stats().level);

  spr.setTextColor(p.body, p.bg);
  spr.setCursor(8, H - 18);
  spr.print(tama.msg);

  spr.setTextSize(1);
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }
  drawStatusBars();
}

// ───────────────────────────────────────────────────────────────────────────
// Touch gesture decoder
// ───────────────────────────────────────────────────────────────────────────
// Pure coordinate-based touch — no "button A/B" zones. The dispatch routes
// taps to specific UI regions (menu rows, approve/deny buttons, page areas)
// using lastTapX/lastTapY in panel coords (0..239 × 0..319).
//
//   short tap: released within 500 ms without moving much → G_TAP
//   long press: held > 600 ms → G_LONG (fires once, suppresses the tap)
enum GestureKind { G_NONE, G_TAP, G_LONG, G_DRAG };

static uint32_t gDownMs       = 0;
static uint32_t gLastSeenMs   = 0;
static bool     gHeld         = false;
static bool     gLongFired    = false;
static uint16_t gDownX        = 0;
static uint16_t gDownY        = 0;
static uint16_t lastTapX      = 0;
static uint16_t lastTapY      = 0;
static int16_t  gDragDy       = 0;

static GestureKind pollGesture() {
  static uint16_t gPrevY = 0;
  uint16_t x, y;
  bool have = touchGetPoint(&x, &y);
  uint32_t now = millis();

  if (have) {
    if (!gHeld) {
      gHeld = true;
      gDownMs = now;
      gLongFired = false;
      gDownX = x; gDownY = y;
      gPrevY = y;
    } else if (!gLongFired) {
      int16_t dy = (int16_t)y - (int16_t)gPrevY;
      gPrevY = y;
      if (abs((int)y - (int)gDownY) > 12) {
        gDragDy = dy;
        gLastSeenMs = now;
        return G_DRAG;
      }
    } else {
      gPrevY = y;
    }
    gLastSeenMs = now;
  }

  if (gHeld && !gLongFired && now - gDownMs > 600) {
    gLongFired = true;
    return G_LONG;
  }

  if (gHeld && !have && now - gLastSeenMs > 100) {
    gHeld = false;
    if (!gLongFired && now - gDownMs < 500 && abs((int)gPrevY - (int)gDownY) <= 12) {
      lastTapX = gDownX;
      lastTapY = gDownY;
      return G_TAP;
    }
  }
  return G_NONE;
}

// ── Menu hit testers ────────────────────────────────────────────────────────
// Must mirror the row geometry in drawMenu / drawSettings / drawReset.
static int hitMenu(int ty) {
  int mh = 20 + MENU_N * 44 + MENU_HINT_H;
  int my = (H - mh) / 2;
  int firstY = my + 18;
  int i = (ty - firstY) / 44;
  return (i >= 0 && i < MENU_N) ? i : -1;
}
static int hitSettings(int ty) {
  int mh = 16 + SETTINGS_N * 36 + MENU_HINT_H;
  int my = (H - mh) / 2;
  int firstY = my + 14;
  int i = (ty - firstY) / 36;
  return (i >= 0 && i < SETTINGS_N) ? i : -1;
}
static int hitReset(int ty) {
  int mh = 20 + RESET_N * 44 + MENU_HINT_H;
  int my = (H - mh) / 2;
  int firstY = my + 18;
  int i = (ty - firstY) / 44;
  return (i >= 0 && i < RESET_N) ? i : -1;
}

static int hitAudio(int ty) {
  int mh = 16 + AUDIO_N * 36 + MENU_HINT_H;
  int my = (H - mh) / 2;
  int firstY = my + 14;
  int i = (ty - firstY) / 36;
  return (i >= 0 && i < AUDIO_N) ? i : -1;
}

void setup() {
  Serial.begin(115200);

  // Mount LittleFS and prime the FS-size cache BEFORE the RGB panel starts.
  // xferRefreshFsUsed() walks the filesystem (a cache-disabling flash read);
  // doing it before displayInit() means it can't starve the not-yet-running
  // panel and fault the RGB ISR.
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  xferRefreshFsUsed();

  displayInit();    // I2C, expander, RGB panel, canvas — all in one call
  audioInit();      // ES8311 + I2S + speaker amp; beep() is a no-op until this runs
  bool pmuOk = powerInit();   // AXP2101 battery/power; getters degrade safely if absent
  Serial.printf("power init: %s\n", pmuOk ? "ok" : "failed");

  startBt();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  applyBrightness();
  petNameLoad();
  buddyInit();
  lastBtSetting = settings().bt;
  if (!settings().bt) bleStopAdvertising();

  characterInit(nullptr);
  gifAvailable = characterLoaded();
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  bool touchOk = touchInit();
  Serial.printf("touch init: %s\n", touchOk ? "ok" : "failed");

  {
    const Palette& p = characterPalette();
    spr.fillScreen(p.bg);
    spr.setTextSize(4);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   drawCenteredString(canvas, line, W/2, H/2 - 24);
      spr.setTextColor(p.body, p.bg);   drawCenteredString(canvas, petName(), W/2, H/2 + 24);
    } else {
      spr.setTextColor(p.body, p.bg);   drawCenteredString(canvas, "Hello!", W/2, H/2 - 24);
      spr.setTextSize(2);
      spr.setTextColor(p.textDim, p.bg);
      drawCenteredString(canvas, "a buddy appears", W/2, H/2 + 20);
    }
    spr.setTextSize(1);
    blit();
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  t++;
  uint32_t now = millis();

  static uint32_t lastPowerPoll = 0;
  if (now - lastPowerPoll > 3000) { lastPowerPoll = now; powerPoll(); }

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // ── BLE advertising control ──────────────────────────────────────────
  bool btOn = settings().bt;
  if (btOn != lastBtSetting) {
    lastBtSetting = btOn;
    if (btOn) {
      bleStartAdvertising();
    } else {
      if (bleConnected()) bleDisconnect();
      bleStopAdvertising();
    }
  }

  // ── Pair action ──────────────────────────────────────────────────
  if (pairTrigger) {
    pairTrigger = false;
    if (!settings().bt) {
      settings().bt = true;
      lastBtSetting = true;
      settingsSave();
    }
    if (bleConnected()) bleDisconnect();
    bleClearBonds();
    if (!bleIsAdvertising()) bleStartAdvertising();
    settingsOpen = false;
    displayMode = DISP_INFO;
    infoPage = 4;
    redrawAll();
    wake();
    Serial.println("[ui] pair triggered");
  }

  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // Prompt arrival: beep, jump to normal, clear overlays — but ONLY for a
  // genuinely new prompt. The bridge re-sends a pending prompt interleaved with
  // heartbeats that omit it, so tama.promptId flickers id→""→id. Prompt ids are
  // unique per request, so we act only when a new NON-EMPTY id differs from the
  // last one we acted on, and we never reset lastPromptId on an empty message.
  // (The old code reset it to "" on every flicker, so the same pending prompt
  // re-fired the beep + menu-close every heartbeat — a constant-beep storm that
  // also slammed any open menu shut, making the volume control unusable.)
  if (tama.promptId[0] && strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    hintScroll = 0;
    promptArrivedMs = millis();
    wake();
    beep(1200, 80);   // prompt arrival
    displayMode = DISP_NORMAL;
    menuOpen = settingsOpen = resetOpen = audioOpen = false;
    applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
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

  if (g == G_DRAG && inPrompt) { hintScroll -= gDragDy; }

  if (g == G_LONG) {
    beep(800, 60);
    if (resetOpen) { resetOpen = false; redrawAll(); }
    else if (audioOpen) { audioOpen = false; redrawAll(); }
    else if (settingsOpen) { settingsOpen = false; redrawAll(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) redrawAll();
    }
  } else if (g == G_TAP) {
    int tx = lastTapX, ty = lastTapY;

    if (inPrompt) {
      // Only the two button rectangles at the bottom fire — the info area
      // above them is non-interactive, so an accidental tap near the top
      // doesn't approve or deny.
      if (ty >= H - 84) {
        bool approve = tx < W / 2;
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
    } else if (audioOpen) {
      // No nav-click here: applyAudio() plays a sample beep at the NEW volume,
      // so each level is heard accurately. A nav-click would fire at the OLD
      // volume first — making "off" (tapped from level 4) play a loud click and
      // reading as louder than level 1. Let the sample tone be the only feedback.
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
      // Home tap cycles the display modes (normal → pet → info → normal).
      displayMode = (displayMode + 1) % DISP_COUNT;
      applyDisplayMode();
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
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
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
    spr.fillScreen(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(2);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setTextSize(3);
      drawCenteredString(canvas, "installing", CX, H/2 - 40);
      spr.setTextSize(2);
      spr.setCursor(12, H/2); spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 24;
      spr.drawRect(12, H/2 + 30, barW, 16, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 2) spr.fillRect(14, H/2 + 32, fill - 2, 12, p.body);
      }
    } else {
      spr.setTextSize(3);
      drawCenteredString(canvas, "no character", CX, H/2);
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
    else if (audioOpen) drawAudio();
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
    Set_Backlight(0);
    screenOff = true;
  }

  delay(screenOff ? 100 : 8);
}
