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
  // Enable only the ADC paths we read. Do NOT reconfigure DCDC/LDO rails -- the
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
