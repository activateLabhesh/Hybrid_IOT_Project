#include "state.h"
#include <Preferences.h>
#include <time.h>
#include "sensors.h"
#include "settings.h"

SemaphoreHandle_t gLock;

void stateBegin() { gLock = xSemaphoreCreateRecursiveMutex(); }

// ---- Time -------------------------------------------------------------------------
bool timeValid() { return time(nullptr) > 1700000000; }

uint32_t nowEpoch() { return timeValid() ? (uint32_t)time(nullptr) : 0; }

static bool localTm(struct tm& out) {
  if (!timeValid()) return false;
  time_t t = time(nullptr);
  localtime_r(&t, &out);
  return true;
}

int localHour() {
  struct tm t;
  return localTm(t) ? t.tm_hour : -1;
}

int localYearDay() {
  struct tm t;
  return localTm(t) ? t.tm_yday : -1;
}

// ---- Events ---------------------------------------------------------------------
struct Event {
  uint32_t t;      // epoch (or 0)
  uint32_t up;     // seconds since boot (fallback when time unknown)
  uint8_t lvl;
  char msg[72];
};
static Event events[EVENT_LOG_SIZE];
static uint16_t evHead = 0, evCount = 0;

void eventLog(EventLevel lvl, const char* fmt, ...) {
  Guard g;
  Event& e = events[evHead];
  e.t = nowEpoch();
  e.up = millis() / 1000;
  e.lvl = lvl;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
  va_end(ap);
  evHead = (evHead + 1) % EVENT_LOG_SIZE;
  if (evCount < EVENT_LOG_SIZE) evCount++;
  Serial.printf("[%s] %s\n", lvl == EV_ALARM ? "ALARM" : lvl == EV_WARN ? "WARN" : "INFO", e.msg);
}

void eventsToJson(JsonArray arr) {
  Guard g;
  // newest first
  for (int i = 0; i < evCount; i++) {
    const Event& e = events[(evHead - 1 - i + EVENT_LOG_SIZE) % EVENT_LOG_SIZE];
    JsonObject o = arr.add<JsonObject>();
    o["t"] = e.t;
    o["up"] = e.up;
    o["lvl"] = e.lvl;
    o["msg"] = e.msg;
  }
}

// ---- Stats ----------------------------------------------------------------------
Stats stats;
static bool statsDirty = false;
static uint32_t lastStatsSaveMs = 0;

void statsLoad() {
  Preferences p;
  p.begin("hf_stats", true);
  bool ok = p.getBytesLength("st") == sizeof(Stats);
  if (ok) p.getBytes("st", &stats, sizeof(Stats));
  p.end();
  if (!ok) statsReset();
}

void statsSave() {
  Preferences p;
  p.begin("hf_stats", false);
  p.putBytes("st", &stats, sizeof(Stats));
  p.end();
  statsDirty = false;
  lastStatsSaveMs = millis();
}

void statsReset() {
  memset(&stats, 0, sizeof(stats));
  stats.avgCycleLitres = 20;
  stats.yday = -1;
  statsSave();
}

void statsAddLitres(float l) {
  if (l <= 0) return;
  stats.totalLitres += l;
  stats.todayLitres += l;
  statsDirty = true;
}

void statsLoop() {
  int yd = localYearDay();
  if (yd >= 0 && yd != stats.yday) {
    if (stats.yday >= 0) {
      memmove(&stats.dailyLitres[1], &stats.dailyLitres[0], sizeof(float) * 13);
      stats.dailyLitres[0] = stats.todayLitres;
      eventLog(EV_INFO, "New day — yesterday used %.1f L", stats.todayLitres);
      stats.todayLitres = 0;
    }
    stats.yday = yd;
    statsDirty = true;
  }
  // Save at most every 10 min to spare the flash (also saved when the pump stops)
  if (statsDirty && millis() - lastStatsSaveMs > 600000UL) statsSave();
}

// ---- History ----------------------------------------------------------------
static HistPoint hist[HISTORY_SIZE];
static uint16_t hHead = 0, hCount = 0;
static uint32_t lastHistMs = 0;
static double lastHistLitres = 0;
static bool histStarted = false;

void historyLoop() {
  uint32_t now = millis();
  if (!histStarted) {
    if (now < 15000) return;  // let sensors settle first
    histStarted = true;
  } else if (now - lastHistMs < HISTORY_INTERVAL_S * 1000UL) {
    return;
  }
  lastHistMs = now;

  Guard g;
  HistPoint& p = hist[hHead];
  p.t = nowEpoch();
  p.moist = sensors.soilOk ? (int8_t)lroundf(sensors.moisturePct) : -1;
  p.tank = sensors.tankOk ? (int8_t)lroundf(sensors.tankPct) : -1;
  p.temp10 = sensors.dhtOk ? (int16_t)lroundf(sensors.tempC * 10) : INT16_MIN;
  p.hum = sensors.dhtOk ? (uint8_t)lroundf(sensors.humidity) : 255;
  p.rain = sensors.raining ? 1 : 0;
  double l = sensors.litresSinceBoot;
  p.litres10 = (uint16_t)min(65535.0, (l - lastHistLitres) * 10.0);
  lastHistLitres = l;
  hHead = (hHead + 1) % HISTORY_SIZE;
  if (hCount < HISTORY_SIZE) hCount++;
}

void historyToJson(JsonObject o) {
  Guard g;
  JsonArray t = o["t"].to<JsonArray>();
  JsonArray m = o["moist"].to<JsonArray>();
  JsonArray k = o["tank"].to<JsonArray>();
  JsonArray tc = o["temp"].to<JsonArray>();
  JsonArray h = o["hum"].to<JsonArray>();
  JsonArray r = o["rain"].to<JsonArray>();
  JsonArray l = o["litres"].to<JsonArray>();
  // oldest first
  uint16_t start = (hHead - hCount + HISTORY_SIZE) % HISTORY_SIZE;
  for (int i = 0; i < hCount; i++) {
    const HistPoint& p = hist[(start + i) % HISTORY_SIZE];
    t.add(p.t);
    if (p.moist >= 0) m.add(p.moist); else m.add(nullptr);
    if (p.tank >= 0) k.add(p.tank); else k.add(nullptr);
    if (p.temp10 != INT16_MIN) tc.add(p.temp10 / 10.0f); else tc.add(nullptr);
    if (p.hum != 255) h.add(p.hum); else h.add(nullptr);
    r.add(p.rain);
    l.add(p.litres10 / 10.0f);
  }
  o["interval"] = HISTORY_INTERVAL_S;
  JsonArray d = o["daily"].to<JsonArray>();
  for (int i = 0; i < 14; i++) d.add(stats.dailyLitres[i]);
}
