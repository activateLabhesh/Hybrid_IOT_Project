#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

enum Mode : uint8_t { MODE_AUTO = 0, MODE_MANUAL = 1 };

// Everything the farmer can change from the portal. Stored in NVS flash.
struct Settings {
  uint32_t magic;

  // Network / security
  char wifiSsid[33];
  char wifiPass[65];
  char adminPass[33];
  char farmName[32];

  // Location (for weather)
  float lat;
  float lon;
  int32_t tzOffsetSec;        // learned from the weather API

  // Irrigation thresholds (soil moisture %)
  uint8_t moistStart;         // start watering below this
  uint8_t moistStop;          // stop watering at/above this
  uint8_t moistCritical;      // crop in danger: ignore forecast/window
  uint16_t maxRunMin;         // max total pump time per auto session
  bool cycleSoak;             // pulse irrigation: run, pause to let water sink in, repeat
  uint8_t cycleRunMin;
  uint8_t soakMin;
  float dailyLimitL;          // 0 = unlimited

  // Tank
  uint8_t tankMinPct;
  float tankEmptyCm;          // sensor-to-water distance when tank is empty
  float tankFullCm;           // ... when tank is full
  float tankCapacityL;

  // Weather-aware logic
  bool weatherEnabled;
  uint8_t rainProbPct;        // forecast probability to consider "rain coming"
  float rainMm;               // minimum forecast rain amount worth waiting for
  uint8_t lookaheadH;         // how far ahead to look
  uint8_t maxWaitH;           // give up waiting for forecast rain after this
  uint16_t rainLockoutMin;    // keep irrigation off after the rain sensor dries
  uint16_t weatherIntervalMin;

  // Watering windows (hours, local time) — avoid midday evaporation
  bool windowEnabled;
  uint8_t w1Start, w1End, w2Start, w2End;

  // Heat-stress boost
  bool heatBoost;
  float heatTempC;
  uint8_t heatBoostPct;

  // Calibration
  uint16_t soilDryRaw;
  uint16_t soilWetRaw;
  uint16_t rainWetRaw;        // AO below this = rain

  // Pump (there is no flow sensor: litres = pumpLpm x run time)
  float pumpLpm;              // pump delivery in litres per minute
  bool relayActiveLow;        // true: relay switches ON with a LOW input (most modules)

  // Protection, done with the tank level sensor
  uint8_t dryRunMin;          // pump on this long but tank not dropping = dry run (0 = off)
  uint8_t leakPct;            // tank drops this % in 30 min with pump off = leak (0 = off)

  uint8_t mode;
};

extern Settings settings;

void settingsLoad();
void settingsSave();
void settingsDefaults(Settings& s);
void settingsToJson(JsonObject o, bool includeSecrets);
// Applies any keys present in `o`. Returns true if WiFi credentials changed.
bool settingsFromJson(JsonObjectConst o);
