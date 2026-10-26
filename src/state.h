#pragma once
// Shared state that is touched from more than one FreeRTOS task
// (main loop, async web server, weather fetcher): lock, event log, statistics.
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"

// ---- Global recursive lock --------------------------------------------------
extern SemaphoreHandle_t gLock;
struct Guard {
  Guard() { xSemaphoreTakeRecursive(gLock, portMAX_DELAY); }
  ~Guard() { xSemaphoreGiveRecursive(gLock); }
};
void stateBegin();

// ---- Time helpers ---------------------------------------------------------------
bool timeValid();
uint32_t nowEpoch();      // 0 when NTP has not synced yet
int localHour();          // -1 when unknown
int localYearDay();       // -1 when unknown

// ---- Event log --------------------------------------------------------------------
enum EventLevel : uint8_t { EV_INFO = 0, EV_WARN = 1, EV_ALARM = 2 };
void eventLog(EventLevel lvl, const char* fmt, ...);
void eventsToJson(JsonArray arr);

// ---- Statistics (persisted) -------------------------------------------------
struct Stats {
  double totalLitres;       // lifetime water used
  double savedLitres;       // estimated water saved by rain-aware skipping
  float todayLitres;
  float avgCycleLitres;     // learned average litres per auto session
  float dailyLitres[14];    // [0] = yesterday
  int16_t yday;
  uint16_t forecastHits;
  uint16_t forecastMisses;
  uint16_t rainSkips;
  uint32_t pumpSeconds;     // lifetime pump runtime (maintenance hint)
};
extern Stats stats;
void statsLoad();
void statsSave();
void statsAddLitres(float l);
void statsLoop();          // day rollover + periodic save
void statsReset();

// ---- History (RAM ring buffer for charts) ---------------------------------
struct HistPoint {
  uint32_t t;
  int8_t moist;
  int8_t tank;
  int16_t temp10;
  uint8_t hum;
  uint8_t rain;
  uint16_t litres10;   // litres used in this interval x10
};
void historyLoop();
void historyToJson(JsonObject o);
