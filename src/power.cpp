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
  pmu.enableTemperatureMeasure();
  pmu.disableTSPinMeasure();   // 4B battery has no NTC; begin() also does this — explicit for clarity
  // Charger configuration (matches the Waveshare 4B AXP2101 demo). Without a
  // valid charge setup the PMIC's battery path stays disengaged and the
  // battery-present detect reads false.
  pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
  pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_400MA);
  pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
  pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
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
  // Percent from the AXP2101's built-in E-Gauge (reg 0xA4). The gauge learns the
  // battery's characteristics automatically over charge/discharge cycles (datasheet
  // section 6.11). On a fresh PMIC reset it may start at 0 and take a cycle to
  // converge; fall back to the voltage curve during that settling period so the
  // display doesn't show 0% on a charged cell.
  int gaugePct = pmu.isBatteryConnect() ? pmu.getBatteryPercent() : -1;
  if (gaugePct > 0) {
    cPct = gaugePct;
  } else {
    // Gauge not ready or no battery: fall back to voltage curve
    cPct = pmu.isBatteryConnect() ? lipoPercent(cMv) : (cUsb ? 100 : lipoPercent(cMv));
  }
  cFull = (pmu.getChargerStatus() == XPOWERS_AXP2101_CHG_DONE_STATE);
  // AXP2101 has no current-sense ADC (datasheet section 6.10: only VBAT, VBUS,
  // VSYS, TS, and die temp). cMa stays 0; callers should not display it.
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
