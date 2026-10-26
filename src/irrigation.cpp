// The decision engine: combines soil moisture, the field rain sensor, the
// weather forecast, tank level and flow feedback to decide when to water.
#include "irrigation.h"
#include "config.h"
#include "sensors.h"
#include "settings.h"
#include "state.h"
#include "weather.h"

enum StatusCode : uint8_t { ST_IDLE = 0, ST_WATERING, ST_WAITING, ST_BLOCKED, ST_ALARM };

// Pump / current run
static bool pumpOn = false;
static RunSource src = SRC_NONE;
static uint32_t runStartMs = 0, pumpOffMs = 0;
static double runStartL = 0;
static float manualTargetL = 0;
static uint32_t manualLimitMs = 0;
static double totalLitres = 0;        // estimated: pumpLpm x run time
static float runStartTankPct = 0;      // for the pump-rate check at stop
static bool runStartTankOk = false;
static uint32_t dryCheckMs = 0;        // tank-based dry-run check window
static float dryCheckPct = 0;

// Auto session (one session = several run/soak cycles until the soil is wet)
static bool sessionActive = false;
static uint32_t sessionPumpMs = 0;
static double sessionStartL = 0;
static float sessionStartMoist = 0;
static uint8_t cycleNo = 0;
static bool soaking = false;
static uint32_t soakUntilMs = 0;

// Rain / forecast logic
static bool waitingRain = false;
static uint32_t waitStartMs = 0;
static uint32_t ignoreForecastUntilMs = 0;
static bool rainDeferred = false;     // we skipped watering because of rain
static bool fcRain = false;
static uint8_t fcProb = 0;
static float fcMm = 0;
static int fcEta = -1;

// Misc
static uint32_t alarms = 0;
static uint32_t pauseUntilMs = 0;
static uint32_t relayTestUntilMs = 0;
static uint32_t leakRefMs = 0;
static float leakRefPct = 0;
static uint32_t lastCtrlMs = 0;
static double lastLitres = 0;
static float effStart = 0, effStop = 0;
static uint8_t effProb = 0;
static bool heatActive = false;
static char statusText[110] = "Starting…";
static uint8_t statusCode = ST_IDLE;

static void setStatus(uint8_t code, const char* fmt, ...) {
  statusCode = code;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(statusText, sizeof(statusText), fmt, ap);
  va_end(ap);
}

static uint8_t relayLevel = LOW;   // level last written to the relay pin

static void pumpPin(bool on) {
  relayLevel = (on != settings.relayActiveLow) ? HIGH : LOW;
  digitalWrite(PIN_PUMP, relayLevel);
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

static void raiseAlarm(uint32_t bit, const char* fmt, ...) {
  if (alarms & bit) return;
  alarms |= bit;
  char msg[72];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  eventLog(EV_ALARM, "%s", msg);
}

static void clearAlarm(uint32_t bit, const char* msg) {
  if (!(alarms & bit)) return;
  alarms &= ~bit;
  if (msg) eventLog(EV_INFO, "%s", msg);
}

static void startPump(RunSource s) {
  pumpOn = true;
  src = s;
  runStartMs = millis();
  runStartL = sensors.litresSinceBoot;
  runStartTankOk = sensors.tankOk;
  runStartTankPct = sensors.tankPct;
  dryCheckMs = runStartMs;
  dryCheckPct = sensors.tankPct;
  pumpPin(true);
}

static void stopPump(const char* why) {
  if (!pumpOn) return;
  pumpPin(false);
  uint32_t ms = millis() - runStartMs;
  float litres = sensors.litresSinceBoot - runStartL;
  if (src == SRC_AUTO) sessionPumpMs += ms;
  stats.pumpSeconds += ms / 1000;
  eventLog(EV_INFO, "Pump OFF (%s) — ~%.1f L in %lu min %lu s", why, litres,
           (unsigned long)(ms / 60000), (unsigned long)((ms / 1000) % 60));
  // Cross-check the pump rate setting against what actually left the tank
  if (ms >= 180000 && runStartTankOk && sensors.tankOk) {
    float tankL = (runStartTankPct - sensors.tankPct) * settings.tankCapacityL / 100.0f;
    if (tankL > 0)
      eventLog(EV_INFO, "Tank dropped %.0f L → pump ≈ %.1f L/min (setting: %.1f)", tankL,
               tankL / (ms / 60000.0f), settings.pumpLpm);
  }
  pumpOn = false;
  src = SRC_NONE;
  pumpOffMs = millis();
  manualTargetL = 0;
  manualLimitMs = 0;
  statsSave();
}

static void endSession(bool completed) {
  if (!sessionActive) return;
  float litres = sensors.litresSinceBoot - sessionStartL;
  if (completed && litres > 1) {
    // learn how much water a typical session takes (used for "water saved")
    stats.avgCycleLitres = stats.avgCycleLitres * 0.7f + litres * 0.3f;
    eventLog(EV_INFO, "Auto irrigation complete: %.1f L, %u cycle(s)", litres, cycleNo);
  }
  sessionActive = false;
  soaking = false;
  cycleNo = 0;
}

static bool inWindow(int h, uint8_t s, uint8_t e) {
  if (s == e) return false;
  return s < e ? (h >= s && h < e) : (h >= s || h < e);
}

static bool inWateringWindow(int* nextHour) {
  int h = localHour();
  if (h < 0) return true;  // time unknown: don't block watering
  if (inWindow(h, settings.w1Start, settings.w1End) || inWindow(h, settings.w2Start, settings.w2End))
    return true;
  for (int i = 1; i <= 24; i++) {
    int hh = (h + i) % 24;
    if (hh == settings.w1Start || hh == settings.w2Start) {
      *nextHour = hh;
      break;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
static void updateDerived(uint32_t now) {
  // Heat-stress boost: hot + dry air means faster evaporation, aim wetter
  heatActive = settings.heatBoost && sensors.dhtOk && sensors.tempC >= settings.heatTempC;
  float boost = heatActive ? settings.heatBoostPct : 0;
  effStart = min(95.0f, settings.moistStart + boost);
  effStop = min(100.0f, settings.moistStop + boost);

  // Adaptive forecast trust: learn from how often forecast rain actually arrived
  int p = settings.rainProbPct;
  uint16_t total = stats.forecastHits + stats.forecastMisses;
  if (total >= 5) {
    float rel = (float)stats.forecastHits / total;
    if (rel < 0.4f) p += 15;        // forecast often wrong here -> demand more certainty
    else if (rel > 0.8f) p -= 10;   // forecast reliable -> trust it earlier
  }
  effProb = constrain(p, 10, 100);

  fcRain = settings.weatherEnabled && now >= ignoreForecastUntilMs &&
           weatherRainExpected(settings.lookaheadH, effProb, settings.rainMm, &fcProb, &fcMm, &fcEta);
}

static void updateAlarms(uint32_t now) {
  bool settled = now > 30000;
  if (settled && !sensors.soilOk) raiseAlarm(AL_SOIL_FAULT, "Soil moisture sensor fault (raw %d)", sensors.soilRaw);
  else if (sensors.soilOk) clearAlarm(AL_SOIL_FAULT, "Soil sensor OK again");

  if (settled && !sensors.dhtOk) raiseAlarm(AL_DHT_FAULT, "DHT11 temperature sensor not responding");
  else if (sensors.dhtOk) clearAlarm(AL_DHT_FAULT, nullptr);

  if (settled && !sensors.tankOk) raiseAlarm(AL_TANK_FAULT, "Tank level sensor (HC-SR04) not responding");
  else if (sensors.tankOk) clearAlarm(AL_TANK_FAULT, nullptr);

  if (sensors.tankOk) {
    if (sensors.tankPct < settings.tankMinPct)
      raiseAlarm(AL_TANK_LOW, "Water tank low: %.0f%%", sensors.tankPct);
    else if (sensors.tankPct > settings.tankMinPct + 5)
      clearAlarm(AL_TANK_LOW, "Tank refilled");
  }

  bool wxBad = settings.weatherEnabled && !weatherFresh() && now > 300000;
  if (wxBad) alarms |= AL_WEATHER;  // informational only
  else alarms &= ~AL_WEATHER;
}

// Tank level falling while the pump is off = leak, tap left open or valve stuck.
static void leakCheck(uint32_t now) {
  if (!settings.leakPct || !sensors.tankOk) { leakRefMs = 0; return; }
  // (re)start the 30-min window after a pump run, at boot, or while the tank is being refilled
  if (!leakRefMs || (pumpOffMs && now - pumpOffMs < 60000) || sensors.tankPct > leakRefPct + 1) {
    leakRefMs = now;
    leakRefPct = sensors.tankPct;
    return;
  }
  if (now - leakRefMs < 30UL * 60000) return;
  float drop = leakRefPct - sensors.tankPct;
  if (drop >= settings.leakPct)
    raiseAlarm(AL_LEAK, "Tank fell %.0f%% in 30 min with pump OFF — leak or valve open?", drop);
  leakRefMs = now;
  leakRefPct = sensors.tankPct;
}

static void whileRunning(uint32_t now) {
  uint32_t elapsed = now - runStartMs;
  float litres = sensors.litresSinceBoot - runStartL;

  // Dry-run protection without a flow sensor: if the pump really delivers
  // water, the tank level must fall. Checked in windows of dryRunMin minutes.
  if (settings.dryRunMin && sensors.tankOk) {
    uint32_t win = settings.dryRunMin * 60000UL;
    if (now - dryCheckMs >= win) {
      float expectPct = settings.pumpLpm * settings.dryRunMin * 100.0f / settings.tankCapacityL;
      float dropPct = dryCheckPct - sensors.tankPct;
      // Only judge when the expected drop is big enough for the sensor to see (~2%)
      if (expectPct >= 2.0f && dropPct < expectPct * 0.25f) {
        raiseAlarm(AL_DRY_RUN, "DRY RUN: pump on %u min but tank level did not drop", settings.dryRunMin);
        stopPump("dry-run protection");
        endSession(false);
        return;
      }
      dryCheckMs = now;
      dryCheckPct = sensors.tankPct;
    }
  }

  if (sensors.tankOk && sensors.tankPct <= TANK_HARD_FLOOR_PCT) {
    raiseAlarm(AL_TANK_LOW, "Tank empty — pump stopped");
    stopPump("tank empty");
    endSession(false);
    return;
  }
  if (elapsed >= HARD_MAX_RUN_MIN * 60000UL) {
    stopPump("hard safety time limit");
    endSession(false);
    return;
  }

  if (src == SRC_MANUAL) {
    if (manualTargetL > 0 && litres >= manualTargetL) stopPump("manual volume reached");
    else if (manualLimitMs && elapsed >= manualLimitMs) stopPump("manual timer done");
    else if (manualTargetL > 0)
      setStatus(ST_WATERING, "Manual watering — %.1f / %.0f L", litres, manualTargetL);
    else
      setStatus(ST_WATERING, "Manual watering — %lu min left",
                (unsigned long)((manualLimitMs - elapsed) / 60000 + 1));
    return;
  }

  // Automatic run
  if (sensors.raining) {
    rainDeferred = true;
    stopPump("rain detected by field sensor");
    endSession(false);
  } else if (!sensors.soilOk) {
    stopPump("soil sensor fault");
    endSession(false);
  } else if (sensors.moisturePct >= effStop) {
    stopPump("target moisture reached");
    endSession(true);
  } else if (sessionPumpMs + elapsed >= settings.maxRunMin * 60000UL) {
    // A resistive probe pulled out of the soil (or a cut probe wire) reads
    // "bone dry" forever. If a full session didn't move the reading, stop
    // trusting it instead of watering to the time limit every session.
    if (sensors.moisturePct - sessionStartMoist < 3)
      raiseAlarm(AL_SOIL_NO_RESPONSE, "Soil reading did not rise after %u min of watering — check the probe",
                 settings.maxRunMin);
    else
      eventLog(EV_WARN, "Max run time reached before soil got wet — check emitters/sensor placement");
    stopPump("max run time");
    endSession(true);
  } else if (settings.dailyLimitL > 0 && stats.todayLitres >= settings.dailyLimitL) {
    stopPump("daily water budget used");
    endSession(true);
  } else if (settings.cycleSoak && elapsed >= settings.cycleRunMin * 60000UL) {
    stopPump("cycle done, soaking");
    soaking = true;
    soakUntilMs = now + settings.soakMin * 60000UL;
  } else {
    setStatus(ST_WATERING, "Auto watering (cycle %u) — soil %.0f%% → target %.0f%%", cycleNo,
              sensors.moisturePct, effStop);
  }
}

static void autoDecide(uint32_t now) {
  // Forecast bookkeeping runs regardless of soil need, to score forecast accuracy
  if (waitingRain) {
    if (sensors.raining) {
      stats.forecastHits++;
      waitingRain = false;
      eventLog(EV_INFO, "Forecast was right — rain arrived");
    } else if (!fcRain) {
      stats.forecastMisses++;
      waitingRain = false;
      eventLog(EV_WARN, "Forecast rain did not arrive — resuming normal watering");
    } else if (now - waitStartMs > settings.maxWaitH * 3600000UL) {
      stats.forecastMisses++;
      waitingRain = false;
      fcRain = false;
      ignoreForecastUntilMs = now + 3 * 3600000UL;
      eventLog(EV_WARN, "Waited %u h for forecast rain — watering anyway", settings.maxWaitH);
    }
  }

  if (settings.mode != MODE_AUTO) {
    setStatus(ST_IDLE, "Manual mode — automatic watering is off");
    return;
  }
  // Let every sensor deliver real readings (and the farmer reach the portal)
  // before the pump may start on its own.
  if (now < BOOT_GRACE_MS) {
    setStatus(ST_WAITING, "Starting up — reading sensors (%lu s)", (unsigned long)((BOOT_GRACE_MS - now) / 1000 + 1));
    return;
  }
  if (alarms & AL_DRY_RUN) {
    setStatus(ST_ALARM, "Pump protection: tank level did not drop while pumping. Check pump & pipes, then acknowledge.");
    return;
  }
  if (alarms & AL_SOIL_NO_RESPONSE) {
    setStatus(ST_ALARM, "Soil reading did not change during watering. Check the probe is firmly in the soil, then acknowledge.");
    return;
  }
  if (pauseUntilMs) {
    if ((int32_t)(pauseUntilMs - now) > 0) {
      setStatus(ST_BLOCKED, "Automation paused — resumes in %lu min",
                (unsigned long)((pauseUntilMs - now) / 60000 + 1));
      return;
    }
    pauseUntilMs = 0;
    eventLog(EV_INFO, "Pause ended — automation resumed");
  }
  if (!sensors.soilOk) {
    endSession(false);
    setStatus(ST_ALARM, "Soil sensor fault — auto watering disabled for safety");
    return;
  }

  float m = sensors.moisturePct;
  bool critical = m < settings.moistCritical;

  if (soaking) {
    if ((int32_t)(soakUntilMs - now) > 0) {
      setStatus(ST_WAITING, "Soaking — letting water sink in, next cycle in %lu min",
                (unsigned long)((soakUntilMs - now) / 60000 + 1));
      return;
    }
    soaking = false;
  }

  bool need = sessionActive ? m < effStop : m < effStart;
  if (!need) {
    if (sessionActive) endSession(true);
    if (rainDeferred && m >= effStart + 5) {
      stats.savedLitres += stats.avgCycleLitres;
      stats.rainSkips++;
      rainDeferred = false;
      eventLog(EV_INFO, "Rain watered the field — saved about %.0f L", stats.avgCycleLitres);
      statsSave();
    }
    setStatus(ST_IDLE, "Soil moisture OK (%.0f%%) — watering starts below %.0f%%", m, effStart);
    return;
  }

  if (sensors.raining) {
    rainDeferred = true;
    endSession(false);
    setStatus(ST_BLOCKED, "Raining in the field — irrigation on hold");
    return;
  }
  uint32_t lockMs = settings.rainLockoutMin * 60000UL;
  if (sensors.lastRainMs && now - sensors.lastRainMs < lockMs && !critical) {
    rainDeferred = true;
    setStatus(ST_WAITING, "Recent rain — letting the soil absorb it (%lu min left)",
              (unsigned long)((lockMs - (now - sensors.lastRainMs)) / 60000 + 1));
    return;
  }
  if (alarms & AL_TANK_LOW) {
    setStatus(ST_BLOCKED, "Tank low (%.0f%%) — refill needed", sensors.tankPct);
    return;
  }
  if (settings.dailyLimitL > 0 && stats.todayLitres >= settings.dailyLimitL) {
    setStatus(ST_BLOCKED, "Daily water budget (%.0f L) used", settings.dailyLimitL);
    return;
  }
  if (fcRain && !critical) {
    if (!waitingRain) {
      waitingRain = true;
      waitStartMs = now;
      eventLog(EV_INFO, "Rain forecast %u%% / %.1f mm within %d h — holding irrigation", fcProb, fcMm,
               fcEta < 0 ? settings.lookaheadH : fcEta);
    }
    rainDeferred = true;
    if (fcEta <= 0)
      setStatus(ST_WAITING, "Rain expected this hour (%u%%, %.1f mm) — saving water & power", fcProb, fcMm);
    else
      setStatus(ST_WAITING, "Rain expected in ~%d h (%u%%, %.1f mm) — saving water & power", fcEta, fcProb, fcMm);
    return;
  }
  int nextH = -1;
  if (settings.windowEnabled && !critical && !inWateringWindow(&nextH)) {
    setStatus(ST_WAITING, "Soil dry (%.0f%%) — waiting for watering window at %02d:00", m, nextH);
    return;
  }
  if (pumpOffMs && now - pumpOffMs < PUMP_MIN_OFF_MS) {
    setStatus(ST_WAITING, "Pump resting before next start");
    return;
  }

  if (!sessionActive) {
    sessionActive = true;
    sessionPumpMs = 0;
    sessionStartL = sensors.litresSinceBoot;
    sessionStartMoist = m;
    cycleNo = 0;
    eventLog(critical ? EV_WARN : EV_INFO, "Auto irrigation started — soil %.0f%%%s", m,
             critical ? " (CRITICAL, overrides forecast/window)" : "");
  }
  cycleNo++;
  rainDeferred = false;
  startPump(SRC_AUTO);
  setStatus(ST_WATERING, "Auto watering (cycle %u)", cycleNo);
}

// ---------------------------------------------------------------------------
void irrigationBegin() {
  pinMode(PIN_PUMP, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  pumpPin(false);
  lastLitres = 0;
}

void irrigationLoop() {
  uint32_t now = millis();
  if (now - lastCtrlMs < CONTROL_INTERVAL_MS) return;
  uint32_t dt = lastCtrlMs ? now - lastCtrlMs : 0;
  lastCtrlMs = now;

  Guard g;
  // Re-assert the relay every tick so a relay-type change in Settings applies at once
  if (relayTestUntilMs && !pumpOn && (int32_t)(relayTestUntilMs - now) > 0) {
    bool on = (now / 2000) % 2;
    pumpPin(on);
    Serial.printf("RELAY TEST: D%d=%s -> relay should be %s\n", PIN_PUMP,
                  relayLevel ? "HIGH" : "LOW", on ? "ON (click, LED lit)" : "OFF (LED dark)");
  } else {
    if (relayTestUntilMs) {
      relayTestUntilMs = 0;
      Serial.println("RELAY TEST: finished");
    }
    pumpPin(pumpOn);
  }

  // No flow sensor: estimate delivered water from the pump's rated flow
  if (pumpOn) totalLitres += settings.pumpLpm * dt / 60000.0;
  sensors.flowLpm = pumpOn ? settings.pumpLpm : 0;
  sensors.litresSinceBoot = totalLitres;
  double L = totalLitres;
  statsAddLitres(L - lastLitres);
  lastLitres = L;

  updateDerived(now);
  updateAlarms(now);

  if (pumpOn) whileRunning(now);
  else leakCheck(now);
  if (!pumpOn) autoDecide(now);
}

// ---- Commands -------------------------------------------------------------
bool irrigationManualStart(float minutes, float litres, String& err) {
  if (alarms & AL_DRY_RUN) { err = "Acknowledge the dry-run alarm first"; return false; }
  if (sensors.tankOk && sensors.tankPct <= TANK_HARD_FLOOR_PCT) { err = "Tank is empty"; return false; }
  if (minutes <= 0 && litres <= 0) minutes = 10;
  minutes = constrain(minutes, 0.0f, (float)HARD_MAX_RUN_MIN);

  if (pumpOn && src == SRC_AUTO) {
    // take over the running auto cycle without restarting the pump
    endSession(false);
    src = SRC_MANUAL;
    runStartMs = millis();
    runStartL = sensors.litresSinceBoot;
  } else if (!pumpOn) {
    if (pumpOffMs && millis() - pumpOffMs < PUMP_MIN_OFF_MS) {
      err = "Pump is resting, try again in a few seconds";
      return false;
    }
    startPump(SRC_MANUAL);
  }
  manualTargetL = litres > 0 ? litres : 0;
  manualLimitMs = minutes > 0 ? (uint32_t)(minutes * 60000) : HARD_MAX_RUN_MIN * 60000UL;
  if (manualTargetL > 0) eventLog(EV_INFO, "Manual watering started: %.0f L", manualTargetL);
  else eventLog(EV_INFO, "Manual watering started: %.0f min", minutes);
  if (sensors.raining) eventLog(EV_WARN, "Note: it is raining in the field");
  return true;
}

void irrigationStop(const char* why) {
  stopPump(why);
  endSession(false);
}

void irrigationSetMode(uint8_t mode) {
  if (mode == settings.mode) return;
  settings.mode = mode;
  settingsSave();
  if (mode != MODE_AUTO && pumpOn && src == SRC_AUTO) irrigationStop("switched to manual mode");
  if (mode != MODE_AUTO) { endSession(false); waitingRain = false; }
  eventLog(EV_INFO, "Mode changed to %s", mode == MODE_AUTO ? "AUTO" : "MANUAL");
}

void irrigationPause(float hours) {
  hours = constrain(hours, 0.1f, 168.0f);
  pauseUntilMs = millis() + (uint32_t)(hours * 3600000UL);
  if (!pauseUntilMs) pauseUntilMs = 1;
  if (pumpOn && src == SRC_AUTO) irrigationStop("automation paused");
  endSession(false);
  eventLog(EV_INFO, "Automation paused for %.1f h", hours);
}

void irrigationResume() {
  if (!pauseUntilMs) return;
  pauseUntilMs = 0;
  eventLog(EV_INFO, "Automation resumed");
}

void irrigationAckAlarms() {
  clearAlarm(AL_DRY_RUN, "Dry-run alarm acknowledged");
  clearAlarm(AL_LEAK, "Leak alarm acknowledged");
  clearAlarm(AL_SOIL_NO_RESPONSE, "Soil probe alarm acknowledged");
  leakRefMs = 0;
}

bool irrigationPumpOn() { return pumpOn; }

void irrigationRelayTest(uint16_t seconds) {
  relayTestUntilMs = seconds ? millis() + seconds * 1000UL : 0;
  if (!relayTestUntilMs && seconds == 0) Serial.println("RELAY TEST: stopped");
}

bool irrigationRelayTestActive() { return relayTestUntilMs != 0; }

bool irrigationRelayPinHigh() { return relayLevel == HIGH; }

void irrigationToJson(JsonObject o) {
  uint32_t now = millis();
  o["mode"] = settings.mode == MODE_AUTO ? "auto" : "manual";
  o["pump"] = pumpOn;
  o["source"] = src == SRC_AUTO ? "auto" : src == SRC_MANUAL ? "manual" : "none";
  o["status"] = statusText;
  o["statusCode"] = statusCode;
  if (pumpOn) {
    o["runSec"] = (now - runStartMs) / 1000;
    o["runLitres"] = sensors.litresSinceBoot - runStartL;
    o["targetL"] = manualTargetL;
    o["limitSec"] = manualLimitMs / 1000;
  }
  o["session"] = sessionActive;
  o["cycle"] = cycleNo;
  o["soaking"] = soaking;
  o["pausedMin"] = pauseUntilMs ? (int32_t)(pauseUntilMs - now) / 60000 + 1 : 0;
  o["effStart"] = effStart;
  o["effStop"] = effStop;
  o["heatBoost"] = heatActive;
  o["waitingRain"] = waitingRain;

  JsonObject f = o["forecast"].to<JsonObject>();
  f["rain"] = fcRain;
  f["prob"] = fcProb;
  f["mm"] = fcMm;
  f["eta"] = fcEta;
  f["needProb"] = effProb;

  o["alarmBits"] = alarms;
  JsonArray a = o["alarms"].to<JsonArray>();
  if (alarms & AL_DRY_RUN) a.add("Dry run — pump stopped (acknowledge to re-enable)");
  if (alarms & AL_LEAK) a.add("Tank level falling while pump is OFF — possible leak");
  if (alarms & AL_TANK_LOW) a.add("Water tank low");
  if (alarms & AL_SOIL_FAULT) a.add("Soil moisture sensor fault");
  if (alarms & AL_SOIL_NO_RESPONSE) a.add("Soil reading didn't rise while watering — probe out of soil or wire cut? (acknowledge to re-enable)");
  if (alarms & AL_DHT_FAULT) a.add("DHT11 sensor not responding");
  if (alarms & AL_TANK_FAULT) a.add("Tank level sensor not responding");
  if (alarms & AL_WEATHER) a.add("Weather forecast unavailable — using field rain sensor only");
}
